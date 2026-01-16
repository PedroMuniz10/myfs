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
#define INODE_AREA_SIZE_SECTORS 20

// --- Estruturas Internas ---

typedef struct {
    unsigned int magic;
    unsigned int blockSize;
    unsigned int numBlocks;
    unsigned int freeBlockStart;
} Superblock;

typedef struct {
    char name[MAX_FILENAME_LENGTH + 1]; 
    unsigned int inode;                 
} DirEntry;

typedef struct {
    Inode *inode;
    unsigned int cursor;
    Disk *disk;
    int inUse;
    int type; 
} MyFS_OpenFile;

static MyFS_OpenFile fdTable[MAX_FDS];

// --- Funções Auxiliares ---

static void saveSuperblock(Disk *d, Superblock *sb) {
    unsigned char buffer[DISK_SECTORDATASIZE];
    memset(buffer, 0, DISK_SECTORDATASIZE);
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

// Leitura interna de inode (para ler diretorio)
static int internalReadInode(Disk *d, Inode *inode, char *buf, unsigned int nbytes, unsigned int offset) {
    Superblock sb;
    loadSuperblock(d, &sb);
    
    unsigned int bytesRead = 0;
    unsigned int fileSize = inodeGetFileSize(inode);
    unsigned int currentPos = offset;

    unsigned int blockSizeBytes = sb.blockSize * DISK_SECTORDATASIZE;

    while (bytesRead < nbytes && currentPos < fileSize) {
        unsigned int blockIdx = currentPos / blockSizeBytes;
        unsigned int blockOffset = currentPos % blockSizeBytes;
        
        unsigned int sectorAddr = inodeGetBlockAddr(inode, blockIdx);
        unsigned char sectorData[DISK_SECTORDATASIZE];
        
        if (sectorAddr == 0) {
            buf[bytesRead] = 0;
        } else {
            unsigned int currentSector = sectorAddr + (blockOffset / DISK_SECTORDATASIZE);
            unsigned int byteInSector = blockOffset % DISK_SECTORDATASIZE;
            diskReadSector(d, currentSector, sectorData);
            buf[bytesRead] = sectorData[byteInSector];
        }
        
        bytesRead++;
        currentPos++;
    }
    return bytesRead;
}

// Busca Inode pelo nome no diretório raiz
static unsigned int findInodeInDir(Disk *d, const char *filename) {
    Inode *root = inodeLoad(ROOT_INODE_NUMBER, d);
    if (!root) return 0;

    unsigned int size = inodeGetFileSize(root);
    unsigned int cursor = 0;
    DirEntry entry;

    while (cursor < size) {
        int n = internalReadInode(d, root, (char*)&entry, sizeof(DirEntry), cursor);
        if (n != sizeof(DirEntry)) break;

        if (entry.inode != 0 && strcmp(entry.name, filename) == 0) {
            free(root);
            return entry.inode;
        }
        cursor += sizeof(DirEntry);
    }
    
    free(root);
    return 0;
}

// Adiciona entrada no diretório
static int addEntryToDir(Disk *d, const char *filename, unsigned int inodeNum) {
    Inode *root = inodeLoad(ROOT_INODE_NUMBER, d);
    Superblock sb;
    loadSuperblock(d, &sb);

    DirEntry newEntry;
    memset(&newEntry, 0, sizeof(DirEntry));
    strncpy(newEntry.name, filename, MAX_FILENAME_LENGTH);
    newEntry.inode = inodeNum;

    unsigned int fileSize = inodeGetFileSize(root);
    unsigned int blockSizeBytes = sb.blockSize * DISK_SECTORDATASIZE;
    
    unsigned int blockIdx = fileSize / blockSizeBytes;
    unsigned int offsetInBlock = fileSize % blockSizeBytes;
    unsigned int sectorAddr = inodeGetBlockAddr(root, blockIdx);

    if (sectorAddr == 0) {
        sectorAddr = sb.freeBlockStart;
        inodeAddBlock(root, sectorAddr);
        sb.freeBlockStart += sb.blockSize;
        saveSuperblock(d, &sb);
    }

    unsigned int targetSector = sectorAddr + (offsetInBlock / DISK_SECTORDATASIZE);
    unsigned char sectorData[DISK_SECTORDATASIZE];
    
    diskReadSector(d, targetSector, sectorData);
    memcpy(&sectorData[offsetInBlock % DISK_SECTORDATASIZE], &newEntry, sizeof(DirEntry));
    diskWriteSector(d, targetSector, sectorData);

    inodeSetFileSize(root, fileSize + sizeof(DirEntry));
    inodeSave(root);
    free(root);
    return 0;
}

// --- Funções da API ---

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
    
    unsigned int startInodes = inodeAreaBeginSector();
    unsigned int dataStart = startInodes + INODE_AREA_SIZE_SECTORS; 
    sb.freeBlockStart = dataStart;
    
    unsigned int totalSectors = diskGetNumSectors(d);
    sb.numBlocks = (totalSectors - dataStart) / (blockSize / DISK_SECTORDATASIZE);

    unsigned char zeroBuf[DISK_SECTORDATASIZE];
    memset(zeroBuf, 0, DISK_SECTORDATASIZE);
    for (unsigned int i = 0; i < INODE_AREA_SIZE_SECTORS; i++) {
        diskWriteSector(d, startInodes + i, zeroBuf);
    }
    // -----------------------

    saveSuperblock(d, &sb);

    Inode *root = inodeCreate(ROOT_INODE_NUMBER, d);
    if (root) {
        inodeSetFileType(root, FILETYPE_DIR);
        inodeSave(root);
        free(root);
    }

    return sb.numBlocks;
}

int myFSxMount(Disk *d, int x) {
    if (x == 1) {
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

    const char *name = (path[0] == '/') ? path + 1 : path;
    
    unsigned int inumber = findInodeInDir(d, name);

    int isNew = 0;

    if (inumber == 0) {
        for (unsigned int i = 2; i < 200; i++) {
            Inode *temp = inodeLoad(i, d);
            if (temp) {
                if (inodeGetFileType(temp) == 0) { 
                    inumber = i;
                    free(temp);
                    break;
                }
                free(temp);
            }
        }
        
        if (inumber == 0) {
            return -1;
        }

        Inode *newInode = inodeCreate(inumber, d);
        if (!newInode) return -1;
        
        inodeSetFileType(newInode, FILETYPE_REGULAR);
        inodeSave(newInode);
        free(newInode);

        if (addEntryToDir(d, name, inumber) != 0) return -1;
        isNew = 1;
    }

    Inode *inodeObj = inodeLoad(inumber, d);
    if (!inodeObj) return -1;

    fdTable[fdIdx].inode = inodeObj;
    fdTable[fdIdx].cursor = 0;
    fdTable[fdIdx].disk = d;
    fdTable[fdIdx].inUse = 1;
    fdTable[fdIdx].type = isNew ? FILETYPE_REGULAR : inodeGetFileType(inodeObj);

    return fdIdx + 1;
}

int myFSRead(int fd, char *buf, unsigned int nbytes) {
    MyFS_OpenFile *file = &fdTable[fd - 1];
    if (!file->inUse) return -1;

    Superblock sb;
    loadSuperblock(file->disk, &sb);
    
    unsigned int blockSizeBytes = sb.blockSize * DISK_SECTORDATASIZE;
    unsigned int bytesRead = 0;
    unsigned int fileSize = inodeGetFileSize(file->inode);

    while (bytesRead < nbytes && file->cursor < fileSize) {
        unsigned int blockIdx = file->cursor / blockSizeBytes;
        unsigned int blockOffset = file->cursor % blockSizeBytes;
        
        unsigned int sectorAddr = inodeGetBlockAddr(file->inode, blockIdx);
        
        if (sectorAddr == 0) {
            buf[bytesRead] = 0;
        } else {
            unsigned int currentSector = sectorAddr + (blockOffset / DISK_SECTORDATASIZE);
            unsigned int byteInSector = blockOffset % DISK_SECTORDATASIZE;
            
            unsigned char sectorData[DISK_SECTORDATASIZE];
            if (diskReadSector(file->disk, currentSector, sectorData) != 0) break;
            
            buf[bytesRead] = sectorData[byteInSector];
        }
        
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

    unsigned int blockSizeBytes = sb.blockSize * DISK_SECTORDATASIZE;
    unsigned int bytesWritten = 0;

    while (bytesWritten < nbytes) {
        unsigned int blockIdx = file->cursor / blockSizeBytes;
        unsigned int blockOffset = file->cursor % blockSizeBytes;

        unsigned int sectorAddr = inodeGetBlockAddr(file->inode, blockIdx);
        
        if (sectorAddr == 0) {
            sectorAddr = sb.freeBlockStart;
            if (inodeAddBlock(file->inode, sectorAddr) == -1) break; 
            
            sb.freeBlockStart += sb.blockSize;
            saveSuperblock(file->disk, &sb);
        }

        unsigned int currentSector = sectorAddr + (blockOffset / DISK_SECTORDATASIZE);
        unsigned int byteInSector = blockOffset % DISK_SECTORDATASIZE;
        
        unsigned char sectorData[DISK_SECTORDATASIZE];
        
        diskReadSector(file->disk, currentSector, sectorData);
        sectorData[byteInSector] = buf[bytesWritten];
        diskWriteSector(file->disk, currentSector, sectorData);

        bytesWritten++;
        file->cursor++;
        
        if (file->cursor > inodeGetFileSize(file->inode)) {
            inodeSetFileSize(file->inode, file->cursor);
        }
    }

    inodeSave(file->inode);
    return bytesWritten;
}

int myFSClose(int fd) {
    if (fd <= 0 || fd > MAX_FDS) return -1;
    if (fdTable[fd - 1].inUse) {
        inodeSave(fdTable[fd - 1].inode);
        free(fdTable[fd - 1].inode); 
        fdTable[fd - 1].inUse = 0;
    }
    return 0;
}

int myFSOpenDir(Disk *d, const char *path) {
    if (path[0] == '/' && path[1] == '\0') {
        int fdIdx = getFreeFd();
        if (fdIdx == -1) return -1;
        
        fdTable[fdIdx].inode = inodeLoad(ROOT_INODE_NUMBER, d);
        fdTable[fdIdx].cursor = 0;
        fdTable[fdIdx].disk = d;
        fdTable[fdIdx].inUse = 1;
        fdTable[fdIdx].type = FILETYPE_DIR;
        return fdIdx + 1;
    }
    return -1;
}

int myFSReadDir(int fd, char *filename, unsigned int *inumber) {
    MyFS_OpenFile *file = &fdTable[fd - 1];
    DirEntry entry;

    while (myFSRead(fd, (char*)&entry, sizeof(DirEntry)) == sizeof(DirEntry)) {
        if (entry.inode != 0) {
            strncpy(filename, entry.name, MAX_FILENAME_LENGTH);
            *inumber = entry.inode;
            return 1;
        }
    }
    return 0; 
}

int myFSLink(int fd, const char *filename, unsigned int inumber) {
    if (fd <= 0 || fd > MAX_FDS || !fdTable[fd-1].inUse) return -1;
    return addEntryToDir(fdTable[fd-1].disk, filename, inumber);
}

int myFSUnlink(int fd, const char *filename) { return 0; }
int myFSCloseDir(int fd) { return myFSClose(fd); }

int installMyFS(void) {
    static FSInfo info;
    info.fsid = 1;
    info.fsname = "MyFS_Final";
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