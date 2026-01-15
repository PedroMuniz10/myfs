#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "myfs.h"
#include "vfs.h"
#include "inode.h"
#include "util.h"

// --- Configurações e Estruturas ---

#define MYFS_MAGIC 0x4D594653 // "MYFS" em Hexadecimal
#define SUPERBLOCK_SECTOR 1
#define ROOT_INODE_NUMBER 1

typedef struct {
    unsigned int magic;
    unsigned int blockSize;
    unsigned int numBlocks;
    unsigned int freeBlockStart;
} Superblock;

// Nome alterado para MyFS_OpenFile para evitar conflito com o Windows.h
typedef struct {
    Inode *inode;
    unsigned int cursor;
    Disk *disk;
    int inUse;
    int type; // FILETYPE_REGULAR ou FILETYPE_DIR
} MyFS_OpenFile;

// Tabela global de ficheiros abertos
static MyFS_OpenFile fdTable[MAX_FDS];

// --- Funções Auxiliares ---

static void saveSuperblock(Disk *d, Superblock *sb) {
    unsigned char buffer[DISK_SECTORDATASIZE];
    ul2char(sb->magic, buffer);
    ul2char(sb->blockSize, buffer + 4);
    ul2char(sb->numBlocks, buffer + 8);
    ul2char(sb->freeBlockStart, buffer + 12);
    diskWriteSector(d, SUPERBLOCK_SECTOR, buffer);
}

static void loadSuperblock(Disk *d, Superblock *sb) {
    unsigned char buffer[DISK_SECTORDATASIZE];
    diskReadSector(d, SUPERBLOCK_SECTOR, buffer);
    char2ul(buffer, &sb->magic);
    char2ul(buffer + 4, &sb->blockSize);
    char2ul(buffer + 8, &sb->numBlocks);
    char2ul(buffer + 12, &sb->freeBlockStart);
}

static int getFreeFd() {
    for (int i = 0; i < MAX_FDS; i++) {
        if (!fdTable[i].inUse) return i;
    }
    return -1;
}

// --- Implementação da API MyFS ---

int myFSIsIdle(Disk *d) {
    for (int i = 0; i < MAX_FDS; i++) {
        if (fdTable[i].inUse && fdTable[i].disk == d) return 0;
    }
    return 1;
}

int myFSFormat(Disk *d, unsigned int blockSize) {
    Superblock sb;
    sb.magic = MYFS_MAGIC;
    sb.blockSize = blockSize;
    
    unsigned int dataStart = inodeAreaBeginSector() + 20; 
    sb.freeBlockStart = dataStart;
    sb.numBlocks = (diskGetNumSectors(d) - dataStart) / (blockSize / DISK_SECTORDATASIZE);

    saveSuperblock(d, &sb);

    Inode *root = inodeCreate(ROOT_INODE_NUMBER, d);
    inodeSetFileType(root, FILETYPE_DIR);
    inodeSave(root);

    return sb.numBlocks;
}

int myFSxMount(Disk *d, int x) {
    if (x == 1) { // Montar
        Superblock sb;
        loadSuperblock(d, &sb);
        if (sb.magic != MYFS_MAGIC) return 0;
        for (int i = 0; i < MAX_FDS; i++) fdTable[i].inUse = 0;
    }
    return 1;
}

int myFSOpen(Disk *d, const char *path) {
    int fdIdx = getFreeFd();
    if (fdIdx == -1) return -1;

    unsigned int inumber = 0;
    // Busca simples por um i-node livre a partir do 2 (1 é a raiz)
    for(unsigned int i = 2; i < 100; i++) {
        Inode *temp = inodeLoad(i, d);
        if (temp == NULL) { inumber = i; break; }
    }

    if (inumber == 0) return -1;

    Inode *newInode = inodeCreate(inumber, d);
    inodeSetFileType(newInode, FILETYPE_REGULAR);
    
    fdTable[fdIdx].inode = newInode;
    fdTable[fdIdx].cursor = 0;
    fdTable[fdIdx].disk = d;
    fdTable[fdIdx].inUse = 1;
    fdTable[fdIdx].type = FILETYPE_REGULAR;

    return fdIdx + 1;
}

int myFSRead(int fd, char *buf, unsigned int nbytes) {
    MyFS_OpenFile *file = &fdTable[fd - 1];
    if (!file->inUse) return -1;

    Superblock sb;
    loadSuperblock(file->disk, &sb);

    unsigned int bytesRead = 0;
    while (bytesRead < nbytes) {
        unsigned int blockIdx = file->cursor / sb.blockSize;
        unsigned int blockOffset = file->cursor % sb.blockSize;
        
        unsigned int sectorAddr = inodeGetBlockAddr(file->inode, blockIdx);
        if (sectorAddr == 0) break; 

        unsigned char sectorData[DISK_SECTORDATASIZE];
        unsigned int currentSector = sectorAddr + (blockOffset / DISK_SECTORDATASIZE);
        diskReadSector(file->disk, currentSector, sectorData);
        
        buf[bytesRead] = sectorData[blockOffset % DISK_SECTORDATASIZE];
        bytesRead++;
        file->cursor++;
    }
    return bytesRead;
}

int myFSWrite(int fd, const char *buf, unsigned int nbytes) {
    MyFS_OpenFile *file = &fdTable[fd - 1];
    if (!file->inUse) return -1;

    Superblock sb;
    loadSuperblock(file->disk, &sb);

    unsigned int bytesWritten = 0;
    while (bytesWritten < nbytes) {
        unsigned int blockIdx = file->cursor / sb.blockSize;
        unsigned int blockOffset = file->cursor % sb.blockSize;

        unsigned int sectorAddr = inodeGetBlockAddr(file->inode, blockIdx);
        if (sectorAddr == 0) {
            sectorAddr = sb.freeBlockStart;
            inodeAddBlock(file->inode, sectorAddr);
            sb.freeBlockStart += (sb.blockSize / DISK_SECTORDATASIZE);
            saveSuperblock(file->disk, &sb);
        }

        unsigned char sectorData[DISK_SECTORDATASIZE];
        unsigned int currentSector = sectorAddr + (blockOffset / DISK_SECTORDATASIZE);
        diskReadSector(file->disk, currentSector, sectorData);
        
        sectorData[blockOffset % DISK_SECTORDATASIZE] = buf[bytesWritten];
        diskWriteSector(file->disk, currentSector, sectorData);

        bytesWritten++;
        file->cursor++;
    }
    inodeSave(file->inode);
    return bytesWritten;
}

int myFSClose(int fd) {
    if (fd <= 0 || fd > MAX_FDS) return -1;
    if (fdTable[fd - 1].inUse) {
        inodeSave(fdTable[fd - 1].inode);
        fdTable[fd - 1].inUse = 0;
    }
    return 0;
}

int myFSOpenDir(Disk *d, const char *path) {
    int fdIdx = getFreeFd();
    if (fdIdx == -1) return -1;

    fdTable[fdIdx].inode = inodeLoad(ROOT_INODE_NUMBER, d);
    fdTable[fdIdx].cursor = 0;
    fdTable[fdIdx].disk = d;
    fdTable[fdIdx].inUse = 1;
    fdTable[fdIdx].type = FILETYPE_DIR;

    return fdIdx + 1;
}

int myFSReadDir(int fd, char *filename, unsigned int *inumber) {
    // Retorna 0 para indicar fim de diretório ou que não há ficheiros listados
    // Necessário implementar a estrutura de entrada de diretório para listagem real
    return 0; 
}

int myFSLink(int fd, const char *filename, unsigned int inumber) {
    return 0;
}

int myFSUnlink(int fd, const char *filename) {
    return 0;
}

int myFSCloseDir(int fd) {
    return myFSClose(fd);
}

int installMyFS(void) {
    static FSInfo info;
    info.fsid = 1;
    info.fsname = "MyFS_Standard";
    info.isidleFn = myFSIsIdle;
    info.formatFn = myFSFormat;
    info.xMountFn = myFSxMount;
    info.openFn = myFSOpen;
    info.readFn = myFSRead;
    info.writeFn = myFSWrite;
    info.closeFn = myFSClose;
    info.opendirFn = myFSOpenDir;
    info.readdirFn = myFSReadDir;
    info.linkFn = myFSLink;
    info.unlinkFn = myFSUnlink;
    info.closedirFn = myFSCloseDir;

    return vfsRegisterFS(&info);
}