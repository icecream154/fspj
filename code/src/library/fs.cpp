// fs.cpp: File System

#include "sfs/fs.h"

#include <algorithm>

#include <assert.h>
#include <stdio.h>
#include <string.h>
using namespace std;
// Debug file system -----------------------------------------------------------

void FileSystem::debug(Disk *disk) {
    Block block;

    // Read Superblock
    disk->read(0, block.Data);

    printf("SuperBlock:\n");
    if (block.Super.MagicNumber == MAGIC_NUMBER) {
        printf("    magic number is valid\n");
    }else{
        printf("    magic number is invalid. exiting...\n");
        return;
    }
    printf("    %u blocks\n"         , block.Super.Blocks);
    printf("    %u inode blocks\n"   , block.Super.InodeBlocks);
    printf("    %u inodes\n"         , block.Super.Inodes);

    // Read Inode blocks
    for(uint32_t blockNum = 1; blockNum < block.Super.InodeBlocks + 1; blockNum++) {
        Block inodeBlock = readBlock(disk, blockNum);

        for(uint32_t i = 0; i < INODES_PER_BLOCK; i++) {

            Inode inode = inodeBlock.Inodes[i];
            if(not inode.Valid) continue;
            //printf("%u\n", i);
            uint32_t inodeNumber = (blockNum-1) * INODES_PER_BLOCK + i;

            printf("Inode %u:\n", inodeNumber);
            printf("    size: %u bytes\n", inode.Size);

            // handle direct blocks
            printf("    direct blocks:");
            for(uint32_t di = 0; di < POINTERS_PER_INODE; di++){
                if(inode.Direct[di] != 0) {
                    printf(" %u", inode.Direct[di]);
                }
            }
            printf("\n");

            // handle indirect blocks
            if(inode.Indirect != 0) {
                printf("    indirect block: %u\n", inode.Indirect);
                printf("    indirect data blocks:");
                Block indirectBlock = readBlock(disk, inode.Indirect);
                for(uint32_t j = 0; j < POINTERS_PER_BLOCK; j++) {
                    if(indirectBlock.Pointers[j] != 0) {
                        printf(" %u", indirectBlock.Pointers[j]);
                    }
                }
                printf("\n");
            }
        }
    }
}

// Format file system ----------------------------------------------------------

bool FileSystem::format(Disk *disk) {
    if(disk->mounted()) return false;
    // Write superblock
    Block super;
    uint32_t allBlocks = disk->size();

    // ten percent and rounding up
    uint32_t newInodeBlocks = (allBlocks % 10 > 0 )? allBlocks / 10 + 1: allBlocks / 10;

    // reformat
    memset(super.Data, 0, disk->BLOCK_SIZE);
    super.Super.MagicNumber = MAGIC_NUMBER;
    super.Super.Blocks = allBlocks;
    super.Super.InodeBlocks = newInodeBlocks;
    super.Super.Inodes = newInodeBlocks * INODES_PER_BLOCK;
    disk->write(0, super.Data);

    // Clear all other blocks
    Block emptyBlock;
    memset(&emptyBlock, 0, sizeof(emptyBlock));
    for(uint32_t i = 1 ; i < allBlocks; i++) {
        disk->write(i, emptyBlock.Data);
    }

    return true;
}

// Mount file system -----------------------------------------------------------

bool FileSystem::mount(Disk *disk) {
    // already mounted before, so mount will fail
    if(disk->mounted()) return false;

    // Read superblock
    Block super = readBlock(disk, 0);
    if (super.Super.MagicNumber != MAGIC_NUMBER) {
        return false;
    } else if(super.Super.Blocks <= 1) {
        return false;
    }
    uint32_t maxInodeBlock = super.Super.Blocks % 10 > 0 ? super.Super.Blocks / 10 + 1 : super.Super.Blocks / 10;
    if(super.Super.InodeBlocks > maxInodeBlock) {
        return false;
    }
    if(super.Super.InodeBlocks * INODES_PER_BLOCK != super.Super.Inodes) {
        return false;
    }

    // Set device and mount
    this->disk = disk;
    this->disk->mount();

    // Copy metadata
    this->inodeBlockSize = super.Super.InodeBlocks;
    this->totalBlockSize = super.Super.Blocks;
    this->dataBlockSize = this->totalBlockSize - this->inodeBlockSize;
    // Allocate free block bitmap

    isBlockAvailable.clear();
    isBlockAvailable.resize(dataBlockSize, true);
    isBlockAvailable.insert(isBlockAvailable.begin(), inodeBlockSize, false);
    for(uint32_t blockNum = 1; blockNum < 1 + inodeBlockSize; blockNum++) {
        Block inodeBlock = readBlock(this->disk, blockNum);
        for(uint32_t i = 0; i < INODES_PER_BLOCK; i++) {
            Inode inode = inodeBlock.Inodes[i];
            if(not inode.Valid) continue;

            // handle the direct and indirect block usage
            for(uint32_t db = 0; db < POINTERS_PER_BLOCK; db++) {
                if(inode.Direct[db] != 0 and inode.Direct[db] < totalBlockSize) {
                    isBlockAvailable[inode.Direct[db]] = false;
                }
            }

            if(inode.Indirect != 0) {
                Block indirectBlock = readBlock(this->disk, inode.Indirect);
                for (uint32_t indb = 0; indb < INODES_PER_BLOCK; indb++) {
                    if (indirectBlock.Pointers[indb] != 0) {
                        isBlockAvailable[indirectBlock.Pointers[indb]] = false;
                    }
                }
            }
        }
    }

    return true;
}

// Create inode ----------------------------------------------------------------

ssize_t FileSystem::create() {
    // Locate free inode in inode table
    ssize_t nextFreeInode = findNextFreeInode();
    if(nextFreeInode == -1) return -1;
    Inode inode;
    inode.Valid = 1;
    inode.Size = 0;
    for(uint32_t di = 0; di < POINTERS_PER_INODE; di++) {
        inode.Direct[di] = 0;
    }
    inode.Indirect = 0;
    writeInode(nextFreeInode, &inode);
    // Record inode if found
    return nextFreeInode;
}

// Remove inode ----------------------------------------------------------------

bool FileSystem::remove(size_t inumber) {
    // Load inode information
    if(inumber >= INODES_PER_BLOCK * inodeBlockSize) return false;
    Inode inode = readInode(inumber);
    if(not inode.Valid) return false;

    // Free direct blocks
    for (uint32_t di = 0; di < POINTERS_PER_INODE; di++) {
        if(inode.Direct[di] != 0) {
            isBlockAvailable[inode.Direct[di]] = true;
            inode.Direct[di] = 0;
        }
    }
    // Free indirect blocks
    if(inode.Indirect != 0) {
        Block indirectBlock = readBlock(this->disk, inode.Indirect);
        for(uint32_t i = 0; i < POINTERS_PER_BLOCK; i++) {
            if(indirectBlock.Pointers[i] != 0) {
                isBlockAvailable[indirectBlock.Pointers[i]] = true;
            }
        }
        isBlockAvailable[inode.Indirect] = true;
    }
    inode.Indirect = 0;


    // Clear inode in inode table
    inode.Valid = 0;
    inode.Size = 0;
    writeInode(inumber, &inode);

    return true;
}

// Inode stat ------------------------------------------------------------------

ssize_t FileSystem::stat(size_t inumber) {
    // Load inode information
    if(inumber >= INODES_PER_BLOCK * inodeBlockSize) return false;
    Inode inode = readInode(inumber);
    if(not inode.Valid) return -1;
    return inode.Size;
}

// Read from inode -------------------------------------------------------------

ssize_t FileSystem::read(size_t inumber, char *data, size_t length, size_t offset) {
    // Load inode information
    Inode inode = readInode(inumber);
    if(not inode.Valid) return -1;

    // Adjust length
    if (offset + length > inode.Size) {
        length = inode.Size - offset;
    }
    if(length < 0) return -1;

    // Read block and copy to data

    size_t bytesRead = 0;
    Block indirectBlock;
    if((offset+length) / disk->BLOCK_SIZE > POINTERS_PER_INODE) {
        if(inode.Indirect == 0) return -1; 
        indirectBlock = readBlock(disk, inode.Indirect);
    }
    while (bytesRead < length) {
        uint32_t nextBlockIndex;
        
        // decide next which block to read
        if((offset+bytesRead) / disk->BLOCK_SIZE < POINTERS_PER_INODE) {
            nextBlockIndex = inode.Direct[(offset+bytesRead) / disk->BLOCK_SIZE];
        }else{
            nextBlockIndex = indirectBlock.Pointers[(offset+bytesRead) / disk->BLOCK_SIZE - POINTERS_PER_INODE];
        }
        if(nextBlockIndex == 0) return -1;
        Block nextBlock = readBlock(disk, nextBlockIndex);

        // reads data from the nextBlock
        size_t bytesCanRead = disk->BLOCK_SIZE - ((offset + bytesRead) % disk->BLOCK_SIZE);
        bytesCanRead = min(bytesCanRead, length - bytesRead);
        memcpy(data + bytesRead, &nextBlock.Data[(offset + bytesRead)%disk->BLOCK_SIZE], bytesCanRead);
        bytesRead += bytesCanRead;
        if(bytesRead == length){
            return bytesRead;
        }
    }
    return bytesRead;
}

// Write to inode --------------------------------------------------------------

ssize_t FileSystem::write(size_t inumber, char *data, size_t length, size_t offset) {
    // Load inode
    Inode inode = readInode(inumber);
    if(not inode.Valid or offset > inode.Size) return -1;

    //adjust the length
    size_t maxFileSize = disk->BLOCK_SIZE * (POINTERS_PER_INODE + POINTERS_PER_BLOCK);
    length = min(length, maxFileSize - offset);
    
    // Write block and copy to data
    size_t bytesWritten = 0;
    Block indirectBlock;

    bool indirectBlockIsRead = false;
    bool inodeModified = false;
    bool indirectBlockModified = false;


    for(uint32_t blockNum = offset / disk->BLOCK_SIZE; bytesWritten < length and blockNum < POINTERS_PER_INODE + POINTERS_PER_BLOCK; blockNum++) {
        uint32_t nextBlockNum;

        if (blockNum < POINTERS_PER_INODE) {
            // should write into the direct blocks
            if (inode.Direct[blockNum] == 0) {
                // need to allocate first
                ssize_t nextAvailableBlock = allocateFreeBlock();
                if(nextAvailableBlock == -1) {
                    break;
                }
                inode.Direct[blockNum] = nextAvailableBlock;
                inodeModified = true;
            }
            nextBlockNum = inode.Direct[blockNum];
        }else {
            // should find a place in indirectBlock and then write
            if(inode.Indirect == 0) {
                ssize_t indirectBlockNum = allocateFreeBlock();
                if(indirectBlockNum == -1) break;
                inode.Indirect = indirectBlockNum;
                inodeModified = true;
            }

            if(not indirectBlockIsRead) {
                indirectBlock = readBlock(disk, inode.Indirect);
                indirectBlockIsRead = true;
            }
            uint32_t nextBlockIndex = blockNum - POINTERS_PER_INODE;
            if(indirectBlock.Pointers[nextBlockIndex] == 0) {
                //need to allocate first
                ssize_t nextAvailableBlock = allocateFreeBlock();
                if(nextAvailableBlock == -1) {
                    break;
                }
                indirectBlock.Pointers[nextBlockIndex] = nextAvailableBlock;
                indirectBlockModified = true;
            }
            nextBlockNum = indirectBlock.Pointers[nextBlockIndex];
        }

        char writeBuffer[disk->BLOCK_SIZE];

        // write the data into the nextBlock
        size_t bytesCanWrite = disk->BLOCK_SIZE - ((offset + bytesWritten) % disk->BLOCK_SIZE);
        bytesCanWrite = min(bytesCanWrite, length - bytesWritten);
        if(bytesCanWrite < disk->BLOCK_SIZE) {
            disk->read(nextBlockNum, (char*)writeBuffer);
        }
        
        memcpy(writeBuffer + ((offset + bytesWritten) % disk->BLOCK_SIZE), data + bytesWritten, bytesCanWrite);
        // persistence
        disk->write(nextBlockNum, writeBuffer);
        bytesWritten += bytesCanWrite;
    }

    //update the size
    uint32_t newSize = max<size_t>(inode.Size, offset + bytesWritten);
    if(newSize != inode.Size) {
        inode.Size = newSize;
        inodeModified = true;
    }

    if(inodeModified) {
        writeInode(inumber, &inode);
    }

    if(indirectBlockModified) {
        disk->write(inode.Indirect, indirectBlock.Data);
    }

    return bytesWritten;
}

FileSystem::Block FileSystem::readBlock(Disk* disk, uint32_t blockNumber) {
    Block block;
    disk->read(blockNumber, block.Data);
    return block;
}

FileSystem::Inode FileSystem::readInode(uint32_t inodeNumber) {

    // get the block number that this inode belongs to
    uint32_t blockNum = inodeNumber / INODES_PER_BLOCK + 1;

    Block block = readBlock(this->disk, blockNum);

    uint32_t startIndex = (inodeNumber % INODES_PER_BLOCK);
    return block.Inodes[startIndex];
}

void FileSystem::writeInode(uint32_t inodeNumber, Inode *newInode) {
    uint32_t blockNum = inodeNumber / INODES_PER_BLOCK + 1;
    Block block = readBlock(this->disk, blockNum);

    block.Inodes[inodeNumber % INODES_PER_BLOCK] = *newInode;

    this->disk->write(blockNum, block.Data);
}

ssize_t FileSystem::findNextFreeInode() {
    for (uint32_t blockNum = 1; blockNum < 1 + inodeBlockSize; blockNum++) {
        Block block = readBlock(this->disk, blockNum);

        for (uint32_t i = 0; i < INODES_PER_BLOCK; i++) {
            if (not block.Inodes[i].Valid) {
                return (blockNum-1) * INODES_PER_BLOCK + i;
            }
        }
    }
    return -1;
}

ssize_t FileSystem::allocateFreeBlock() {
    int blockId = -1;
    for(uint32_t i = 1+inodeBlockSize; i < totalBlockSize; i++) {
        if(isBlockAvailable[i]) {
            isBlockAvailable[i] = 0;
            blockId = i;
            break;
        }
    }

    if(blockId != -1) {
        char data[disk->BLOCK_SIZE];
        memset(data, 0, disk->BLOCK_SIZE);
        disk->write(blockId, (char*)data);
    }
    return blockId;
}