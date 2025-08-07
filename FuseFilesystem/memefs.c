/*
 * memefs.c - MEMEFS FUSE implementation for CMSC 421 Project 4
 */

#define FUSE_USE_VERSION 35
#include <fuse3/fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <arpa/inet.h>  // For byte order conversion
#include <ctype.h>
#include <time.h>
#include <assert.h>

/* Filesystem constants */
#define BLOCK_SIZE 512
#define TOTAL_BLOCKS 256
#define DIR_ENTRY_SIZE 32
#define MAX_FILENAME 8
#define MAX_EXTENSION 3
#define DIR_ENTRIES_PER_BLOCK (BLOCK_SIZE / DIR_ENTRY_SIZE)
#define FILE_TYPE_REGULAR 0x8000  // Regular file type flag
#define FILE_PERMISSIONS 0x01FF   // rwxrwxrwx (0777)
#define END_OF_CHAIN 0xFFFF       // FAT end-of-chain marker
#define BCD_MASK 0xFF             // Mask for BCD digits
#define MIN(a, b) ((a) < (b) ? (a) : (b))

/* Debug mode - set to 0 for production */
#define DEBUG 1

/* Error checking macros */
#define CHECK_NULL(ptr, msg) if ((ptr) == NULL) { perror(msg); return -1; }
#define CHECK_SYS(call, msg) if ((call) == -1) { perror(msg); return -1; }
#define CHECK_FS(call, msg) if ((call) != 0) { fprintf(stderr, "%s\n", msg); return -1; }

/* Superblock structure */
typedef struct {
    char signature[16];      // "?MEMEFS++CMSC421"
    uint8_t mounted;         // Mount status flag
    uint8_t reserved[3];     // Padding bytes
    uint32_t version;        // Filesystem version
    uint8_t creation_time[8]; // BCD timestamp
    uint16_t main_fat_block; // Block number of main FAT
    uint16_t main_fat_size;  // Size of main FAT in blocks
    uint16_t backup_fat_block; // Block number of backup FAT
    uint16_t backup_fat_size; // Size of backup FAT in blocks
    uint16_t dir_start_block; // Starting block of directory
    uint16_t dir_size;       // Size of directory in blocks
    uint16_t user_blocks_count; // Number of user blocks
    uint16_t first_user_block;  // First user block number
    char volume_label[16];   // Volume name
    uint8_t unused[448];     // Padding to fill block
} Superblock;

/* Directory entry structure (32 bytes) */
typedef struct {
    uint16_t type_permission; // File type and UNIX permissions
    uint16_t start_block;    // First block of file data
    char filename[11];       // 8.3 filename format
    uint8_t unused;          // Padding byte
    uint8_t timestamp[8];    // BCD modification time
    uint32_t file_size;      // File size in bytes
    uint16_t owner_uid;      // Owner user ID
    uint16_t group_gid;      // Owner group ID
} DirEntry;

/* Global filesystem state */
static int img_fd = -1;          // File descriptor for disk image
static Superblock sb;            // Loaded superblock
static uint16_t fat[TOTAL_BLOCKS]; // File Allocation Table
static DirEntry *dir_entries = NULL; // Directory entries array
static int dir_entry_count = 0;   // Number of valid directory entries

/* Helper functions for byte order conversion (page 3 specifies big-endian) */
static inline uint16_t from_be16(uint16_t val) { return ntohs(val); }
static inline uint32_t from_be32(uint32_t val) { return ntohl(val); }
static inline uint16_t to_be16(uint16_t val) { return htons(val); }
static inline uint32_t to_be32(uint32_t val) { return htonl(val); }

/* Convert number to BCD format */
static uint8_t to_bcd(uint8_t num) {
    if (num > 99) return 0xFF;  // Safety check
    return ((num / 10) << 4) | (num % 10);
}

/* Generate current timestamp in BCD format */
static void generate_timestamp(uint8_t bcd_time[8]) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    
    
    bcd_time[0] = to_bcd(20);  // Hardcoded 21st century
    bcd_time[1] = to_bcd(tm->tm_year % 100);
    bcd_time[2] = to_bcd(tm->tm_mon + 1);
    bcd_time[3] = to_bcd(tm->tm_mday);
    bcd_time[4] = to_bcd(tm->tm_hour);
    bcd_time[5] = to_bcd(tm->tm_min);
    bcd_time[6] = to_bcd(tm->tm_sec);
    bcd_time[7] = 0;  // Reserved byte
}

/* Validate filename characters */
static int is_valid_filename_char(char c) {
    return isalnum(c) || c == '^' || c == '~' || c == '_' || c == '=' || c == '|';
}

/* Validate complete filename  */
static int validate_filename(const char *filename) {
    for (int i = 0; i < 11 && filename[i]; i++) {
        if (!is_valid_filename_char(filename[i])) {
            return 0;
        }
    }
    return 1;
}

/* Write FAT to disk (both main and backup) */
static int write_fat() {
    uint16_t fat_be[TOTAL_BLOCKS];
    
    // Convert to big-endian for storage
    for (int i = 0; i < TOTAL_BLOCKS; i++) {
        fat_be[i] = to_be16(fat[i]);
    }
    
    // Write main FAT
    if (lseek(img_fd, sb.main_fat_block * BLOCK_SIZE, SEEK_SET) == -1 ||
        write(img_fd, fat_be, sizeof(fat_be)) != sizeof(fat_be)) {
        perror("Failed to write main FAT");
        return -1;
    }
    
    // Write backup FAT
    if (lseek(img_fd, sb.backup_fat_block * BLOCK_SIZE, SEEK_SET) == -1 ||
        write(img_fd, fat_be, sizeof(fat_be)) != sizeof(fat_be)) {
        perror("Failed to write backup FAT");
        return -1;
    }
    
    return 0;
}

/* Write directory entries to disk */
static int write_directory() {
    uint8_t dir_data[BLOCK_SIZE];
    int entries_written = 0;
    int dir_blocks = sb.dir_size;
    
    // Write directory blocks in reverse order (blocks 253 down to 240)
    for (int block = sb.dir_start_block; block > sb.dir_start_block - dir_blocks; block--) {
        memset(dir_data, 0, BLOCK_SIZE);
        
        // Pack directory entries into block
        for (int i = 0; i < DIR_ENTRIES_PER_BLOCK && entries_written < dir_entry_count; i++) {
            DirEntry entry = dir_entries[entries_written];
            
            // Convert fields to big-endian
            entry.type_permission = to_be16(entry.type_permission);
            entry.start_block = to_be16(entry.start_block);
            entry.file_size = to_be32(entry.file_size);
            entry.owner_uid = to_be16(entry.owner_uid);
            entry.group_gid = to_be16(entry.group_gid);
            
            // Copy entry to block buffer
            memcpy(dir_data + i * DIR_ENTRY_SIZE, &entry, DIR_ENTRY_SIZE);
            entries_written++;
        }
        
        // Write directory block
        if (lseek(img_fd, block * BLOCK_SIZE, SEEK_SET) == -1 ||
            write(img_fd, dir_data, BLOCK_SIZE) != BLOCK_SIZE) {
            perror("Failed to write directory block");
            return -1;
        }
    }
    
    return 0;
}

/* Find file by path in directory entries */
static DirEntry* find_file(const char *path) {
    if (strcmp(path, "/") == 0) return NULL;  // Root directory
    
    // Extract filename from path
    const char *filename = strrchr(path, '/');
    if (!filename) return NULL;
    filename++; // Skip the '/'
    
    // Search directory entries
    for (int i = 0; i < dir_entry_count; i++) {
        if (dir_entries[i].type_permission == 0) continue;  // Skip empty entries
        
        // Format filename as "NAME.EXT"
        char entry_name[13];
        snprintf(entry_name, sizeof(entry_name), "%.8s.%.3s", 
                dir_entries[i].filename, dir_entries[i].filename + 8);
        
        if (strcmp(filename, entry_name) == 0) {
            return &dir_entries[i];
        }
    }
    return NULL;
}

/* Load superblock from disk */
static int load_superblock() {
    // Read primary superblock (block 255)
    if (lseek(img_fd, 255 * BLOCK_SIZE, SEEK_SET) == -1) {
        perror("Failed to seek to superblock");
        return -1;
    }

    if (read(img_fd, &sb, sizeof(sb)) != sizeof(sb)) {
        perror("Failed to read superblock");
        return -1;
    }
    
    // Validate signature
    if (memcmp(sb.signature, "?MEMEFS++CMSC421", 16) != 0) {
        fprintf(stderr, "Invalid MEMEFS signature\n");
        return -1;
    }
    
    // Convert fields from big-endian
    sb.version = from_be32(sb.version);
    sb.main_fat_block = from_be16(sb.main_fat_block);
    sb.main_fat_size = from_be16(sb.main_fat_size);
    sb.backup_fat_block = from_be16(sb.backup_fat_block);
    sb.backup_fat_size = from_be16(sb.backup_fat_size);
    sb.dir_start_block = from_be16(sb.dir_start_block);
    sb.dir_size = from_be16(sb.dir_size);
    sb.user_blocks_count = from_be16(sb.user_blocks_count);
    sb.first_user_block = from_be16(sb.first_user_block);
    
    // Validate superblock values
    if (sb.main_fat_block >= TOTAL_BLOCKS || sb.backup_fat_block >= TOTAL_BLOCKS ||
        sb.dir_start_block >= TOTAL_BLOCKS || sb.first_user_block >= TOTAL_BLOCKS) {
        fprintf(stderr, "Invalid block numbers in superblock\n");
        return -1;
    }
    
    return 0;
}

/* Load FAT from disk */
static int load_fat() {
    // Read main FAT
    if (lseek(img_fd, sb.main_fat_block * BLOCK_SIZE, SEEK_SET) == -1) {
        perror("Failed to seek to FAT");
        return -1;
    }

    if (read(img_fd, fat, sizeof(fat)) != sizeof(fat)) {
        perror("Failed to read FAT");
        return -1;
    }

    // Convert from big-endian
    for (int i = 0; i < TOTAL_BLOCKS; i++) {
        fat[i] = from_be16(fat[i]);
    }

    return 0;
}

/* Load directory entries from disk */
static int load_directory() {
    int dir_blocks = sb.dir_size;
    uint8_t *dir_data = malloc(dir_blocks * BLOCK_SIZE);
    CHECK_NULL(dir_data, "Failed to allocate directory memory");

    // Read directory blocks in reverse order (253 down to 240)
    uint8_t *current_pos = dir_data;
    int end_block = sb.dir_start_block - dir_blocks + 1;
    
    for (int block = sb.dir_start_block; block >= end_block; block--) {
        if (lseek(img_fd, block * BLOCK_SIZE, SEEK_SET) == -1) {
            perror("Directory seek failed");
            free(dir_data);
            return -1;
        }
        
        ssize_t bytes_read = read(img_fd, current_pos, BLOCK_SIZE);
        if (bytes_read != BLOCK_SIZE) {
            fprintf(stderr, "Failed to read directory block %d\n", block);
            free(dir_data);
            return -1;
        }
        current_pos += BLOCK_SIZE;
    }

    // Parse directory entries
    dir_entry_count = (dir_blocks * BLOCK_SIZE) / DIR_ENTRY_SIZE;
    dir_entries = malloc(dir_entry_count * sizeof(DirEntry));
    if (!dir_entries) {
        perror("Failed to allocate entries memory");
        free(dir_data);
        return -1;
    }

    // Process entries and validate them
    int valid_entries = 0;
    for (int i = 0; i < dir_entry_count; i++) {
        DirEntry *entry = (DirEntry *)(dir_data + i * DIR_ENTRY_SIZE);
        
        // Convert from big-endian
        entry->type_permission = from_be16(entry->type_permission);
        entry->start_block = from_be16(entry->start_block);
        entry->file_size = from_be32(entry->file_size);
        entry->owner_uid = from_be16(entry->owner_uid);
        entry->group_gid = from_be16(entry->group_gid);

        // Skip empty entries
        if (entry->type_permission == 0) {
            continue;
        }

        // Validate filename (page 9)
        if (!validate_filename(entry->filename)) {
            continue;
        }

        // Validate start block if file is not empty
        if (entry->file_size > 0 && 
            (entry->start_block < sb.first_user_block || entry->start_block >= TOTAL_BLOCKS)) {
            fprintf(stderr, "Invalid start block %u for file %.*s\n",
                   entry->start_block, 11, entry->filename);
            continue;
        }

        // Copy valid entry
        memcpy(&dir_entries[valid_entries++], entry, sizeof(DirEntry));
    }

    dir_entry_count = valid_entries;
    free(dir_data);
    return 0;
}

/* FUSE operation: get file attributes */
static int memefs_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi) {
    (void)fi;
    if (DEBUG) fprintf(stderr, "DEBUG: getattr called for path: %s\n", path);
    memset(stbuf, 0, sizeof(struct stat));
    
    if (strcmp(path, "/") == 0) {
        // Root directory attributes
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        stbuf->st_size = BLOCK_SIZE;
        stbuf->st_uid = getuid();
        stbuf->st_gid = getgid();
        stbuf->st_atime = stbuf->st_mtime = stbuf->st_ctime = time(NULL);
        return 0;
    }
    
    // File attributes
    DirEntry *entry = find_file(path);
    if (entry) {
        stbuf->st_mode = S_IFREG | (entry->type_permission & 0777);
        stbuf->st_nlink = 1;
        stbuf->st_size = entry->file_size;
        stbuf->st_uid = entry->owner_uid;
        stbuf->st_gid = entry->group_gid;
        stbuf->st_atime = stbuf->st_mtime = stbuf->st_ctime = time(NULL);
        return 0;
    }
    
    return -ENOENT;
}

/* FUSE operation: read directory contents */
static int memefs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                         off_t offset, struct fuse_file_info *fi,
                         enum fuse_readdir_flags flags) {
    (void)offset;
    (void)fi;
    (void)flags;
    
    if (DEBUG) fprintf(stderr, "DEBUG: readdir called for path: %s\n", path);
    if (strcmp(path, "/") != 0) return -ENOENT;
    
    // Always include . and ..
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    
    // Add all valid files in 8.3 format
    for (int i = 0; i < dir_entry_count; i++) {
        if (dir_entries[i].type_permission == 0) continue;
        
        char filename[13];
        snprintf(filename, sizeof(filename), "%.8s.%.3s", 
                dir_entries[i].filename, dir_entries[i].filename + 8);
        filler(buf, filename, NULL, 0, 0);
    }
    
    return 0;
}

/* FUSE operation: open file */
static int memefs_open(const char *path, struct fuse_file_info *fi) {
    if (DEBUG) fprintf(stderr, "DEBUG: open called for path: %s\n", path);
    
    if (strcmp(path, "/") == 0) return -EISDIR;
    
    DirEntry *entry = find_file(path);
    if (!entry) return -ENOENT;
    
    // Check write permissions if opening for write
    if ((fi->flags & O_ACCMODE) != O_RDONLY) {
        if (!(entry->type_permission & 0222)) {
            return -EACCES;
        }
    }
    
    return 0;
}

/* FUSE operation: read file data */
static int memefs_read(const char *path, char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi) {
    (void)fi;
    if (DEBUG) fprintf(stderr, "DEBUG: read called for path: %s\n", path);
    
    DirEntry *entry = find_file(path);
    if (!entry) {
        if (DEBUG) fprintf(stderr, "File not found: %s\n", path);
        return -ENOENT;
    }
    
    // Handle empty file or read beyond EOF
    uint32_t file_size = entry->file_size;
    if (file_size == 0 || offset >= (off_t)file_size) {
        if (DEBUG) fprintf(stderr, "Read beyond EOF or empty file\n");
        return 0;
    }
    
    // Adjust read size if it would go beyond EOF
    if (offset + (off_t)size > (off_t)file_size) {
        size = file_size - offset;
        if (DEBUG) fprintf(stderr, "Adjusted read size to %zu\n", size);
    }

    // Follow FAT chain to read data blocks
    uint16_t current_block = entry->start_block;
    if (current_block == END_OF_CHAIN) {
        return 0; // Empty file
    }

    size_t total_read = 0;
    off_t current_offset = offset;
    
    while (size > 0 && current_block != END_OF_CHAIN) {
        if (current_block >= TOTAL_BLOCKS || current_block < sb.first_user_block) {
            fprintf(stderr, "ERROR: Invalid block %u in FAT chain\n", current_block);
            return -EIO;
        }
        
        // Calculate position within current block
        off_t block_offset = current_block * BLOCK_SIZE;
        off_t pos_in_block = current_offset % BLOCK_SIZE;
        size_t bytes_avail = BLOCK_SIZE - pos_in_block;
        size_t to_read = MIN(size, bytes_avail);

        // Read data from block
        if (lseek(img_fd, block_offset + pos_in_block, SEEK_SET) == -1) {
            perror("Seek failed");
            return -EIO;
        }

        ssize_t bytes_read = read(img_fd, buf + total_read, to_read);
        if (bytes_read < 0) {
            perror("Read failed");
            return -EIO;
        }

        total_read += bytes_read;
        current_offset += bytes_read;
        size -= bytes_read;

        // Move to next block if we've read entire block
        if (pos_in_block + bytes_read == BLOCK_SIZE) {
            current_block = fat[current_block];
        }
    }
    
    if (DEBUG) fprintf(stderr, "Completed read: %zu bytes\n", total_read);
    return total_read;
}

/* FUSE operation: create new file */
static int memefs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    if (DEBUG) fprintf(stderr, "DEBUG: create called for path: %s\n", path);

    (void)mode;
    (void)fi;
    
    if (find_file(path)) return -EEXIST;

    // Find free directory entry
    int free_entry = -1;
    for (int i = 0; i < dir_entry_count; i++) {
        if (dir_entries[i].type_permission == 0) {
            free_entry = i;
            break;
        }
    }
    if (free_entry == -1) return -ENOSPC;

    // Find free block for file data
    uint16_t free_block = 0;
    for (int i = sb.first_user_block; i < sb.first_user_block + sb.user_blocks_count; i++) {
        if (fat[i] == 0) {
            free_block = i;
            break;
        }
    }
    if (free_block == 0) return -ENOSPC;

    // Parse filename into 8.3 format (page 9)
    const char *filename = strrchr(path, '/') + 1;
    const char *dot = strchr(filename, '.');
    char base[MAX_FILENAME + 1] = {0};
    char ext[MAX_EXTENSION + 1] = {0};
    
    if (dot && (dot - filename) <= MAX_FILENAME && strlen(dot + 1) <= MAX_EXTENSION) {
        strncpy(base, filename, dot - filename);
        strncpy(ext, dot + 1, MAX_EXTENSION);
    } else {
        strncpy(base, filename, MAX_FILENAME);
    }

    // Initialize directory entry
    DirEntry *entry = &dir_entries[free_entry];
    memset(entry, 0, sizeof(DirEntry));
    entry->type_permission = FILE_TYPE_REGULAR | (mode & FILE_PERMISSIONS);
    entry->start_block = free_block;
    
    // Format filename in 8.3 format (space padded)
    memset(entry->filename, ' ', 11);
    strncpy(entry->filename, base, MAX_FILENAME);
    if (ext[0]) {
        strncpy(entry->filename + 8, ext, MAX_EXTENSION);
    }

    // Set metadata
    entry->file_size = 0;
    generate_timestamp(entry->timestamp);
    entry->owner_uid = getuid();
    entry->group_gid = getgid();
    
    // Mark block as used in FAT
    fat[free_block] = END_OF_CHAIN;
    
    // Clear the allocated block
    uint8_t zero_block[BLOCK_SIZE] = {0};
    if (lseek(img_fd, free_block * BLOCK_SIZE, SEEK_SET) == -1 ||
        write(img_fd, zero_block, BLOCK_SIZE) != BLOCK_SIZE) {
        perror("Failed to clear new block");
        return -EIO;
    }

    // Persist changes to disk
    if (write_directory() < 0 || write_fat() < 0) {
        return -EIO;
    }
    
    return 0;
}

/* FUSE operation: write file data */
static int memefs_write(const char *path, const char *buf, size_t size, off_t offset,
                       struct fuse_file_info *fi) {
    if (DEBUG) fprintf(stderr, "DEBUG: write called for path: %s\n", path);
    (void)fi;
    
    DirEntry *entry = find_file(path);
    if (!entry) return -ENOENT;

    // Calculate required blocks
    uint32_t required_blocks = (offset + size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    uint32_t current_blocks = (entry->file_size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    
    // Allocate additional blocks if needed
    if (required_blocks > current_blocks) {
        uint16_t last_block = entry->start_block;
        if (last_block == END_OF_CHAIN) {
            // Find first free block for empty file
            for (int i = sb.first_user_block; i < sb.first_user_block + sb.user_blocks_count; i++) {
                if (fat[i] == 0) {
                    last_block = i;
                    entry->start_block = i;
                    fat[i] = END_OF_CHAIN;
                    break;
                }
            }
            if (last_block == END_OF_CHAIN) return -ENOSPC;
        } else {
            // Find last block in chain
            while (fat[last_block] != END_OF_CHAIN) {
                last_block = fat[last_block];
            }
        }
        
        // Allocate new blocks
        for (uint32_t i = current_blocks; i < required_blocks; i++) {
            uint16_t new_block = 0;
            for (int j = sb.first_user_block; j < sb.first_user_block + sb.user_blocks_count; j++) {
                if (fat[j] == 0) {
                    new_block = j;
                    break;
                }
            }
            if (new_block == 0) return -ENOSPC;
            
            fat[last_block] = new_block;
            fat[new_block] = END_OF_CHAIN;
            last_block = new_block;
            
            // Clear new block
            if (lseek(img_fd, new_block * BLOCK_SIZE, SEEK_SET) == -1 ||
                write(img_fd, (uint8_t[BLOCK_SIZE]){0}, BLOCK_SIZE) != BLOCK_SIZE) {
                return -EIO;
            }
        }
    }

    // Perform the write operation
    uint16_t current_block = entry->start_block;
    off_t remaining_offset = offset;
    const char *current_buf = buf;
    size_t remaining_size = size;
    
    // Find starting block
    while (remaining_offset >= BLOCK_SIZE && current_block != END_OF_CHAIN) {
        current_block = fat[current_block];
        remaining_offset -= BLOCK_SIZE;
    }
    
    // Write data block by block
    while (remaining_size > 0 && current_block != END_OF_CHAIN) {
        size_t chunk_size = MIN(remaining_size, (size_t)(BLOCK_SIZE - remaining_offset));
        
        if (lseek(img_fd, current_block * BLOCK_SIZE + remaining_offset, SEEK_SET) == -1 ||
            write(img_fd, current_buf, chunk_size) != (ssize_t)chunk_size) {
            return -EIO;
        }
        
        current_buf += chunk_size;
        remaining_size -= chunk_size;
        remaining_offset = 0;
        current_block = fat[current_block];
    }
    
    // Update metadata
    if (offset + size > entry->file_size) {
        entry->file_size = offset + size;
    }
    generate_timestamp(entry->timestamp);
    
    // Persist changes
    if (write_directory() < 0 || write_fat() < 0) {
        return -EIO;
    }
    
    return size;
}

/* FUSE operation: truncate file (optional for Part 2) */
static int memefs_truncate(const char *path, off_t size, struct fuse_file_info *fi) {
    if (DEBUG) fprintf(stderr, "DEBUG: truncate called for path: %s\n", path);
    (void)fi;
    
    DirEntry *entry = find_file(path);
    if (!entry) return -ENOENT;
    
    uint32_t current_size = entry->file_size;
    if (size == current_size) return 0;
    
    if (size > current_size) {
        // Extend file - let write handle allocation
        return memefs_write(path, NULL, 0, size, NULL);
    } else {
        // Shrink file - free excess blocks
        uint16_t block = entry->start_block;
        uint32_t blocks_to_keep = (size + BLOCK_SIZE - 1) / BLOCK_SIZE;
        uint32_t current_block = 0;
        
        // Find where to truncate the chain
        uint16_t prev_block = END_OF_CHAIN;
        while (block != END_OF_CHAIN && current_block < blocks_to_keep) {
            prev_block = block;
            block = fat[block];
            current_block++;
        }
        
        // Free remaining blocks
        if (block != END_OF_CHAIN) {
            uint16_t next_block;
            while (block != END_OF_CHAIN) {
                next_block = fat[block];
                fat[block] = 0; // Mark as free
                block = next_block;
            }
            
            // Terminate the chain
            if (prev_block != END_OF_CHAIN) {
                fat[prev_block] = END_OF_CHAIN;
            } else {
                entry->start_block = END_OF_CHAIN;
            }
        }
        
        entry->file_size = size;
        generate_timestamp(entry->timestamp);
        
        // Persist changes
        if (write_directory() < 0 || write_fat() < 0) {
            return -EIO;
        }
    }
    
    return 0;
}

/* FUSE operation: delete file */
static int memefs_unlink(const char *path) {
    if (DEBUG) fprintf(stderr, "DEBUG: unlink called for path: %s\n", path);
    
    DirEntry *entry = find_file(path);
    if (!entry) return -ENOENT;
    
    // Free all blocks in FAT chain
    uint16_t block = entry->start_block;
    while (block != END_OF_CHAIN) {
        uint16_t next_block = fat[block];
        fat[block] = 0; // Mark as free
        block = next_block;
    }
    
    // Mark directory entry as free
    entry->type_permission = 0;
    
    // Persist changes
    if (write_directory() < 0 || write_fat() < 0) {
        return -EIO;
    }
    
    return 0;
}

/* FUSE operation: update timestamps (optional) */
static int memefs_utimens(const char *path, const struct timespec ts[2],
                         struct fuse_file_info *fi) {
    if (DEBUG) fprintf(stderr, "DEBUG: utimens called for path: %s\n", path);
    (void)fi;
    (void)ts;
    
    DirEntry *entry = find_file(path);
    if (!entry) return -ENOENT;
    
    // Update modification time
    generate_timestamp(entry->timestamp);
    
    if (write_directory() < 0) {
        return -EIO;
    }
    
    return 0;
}

/* FUSE operations structure */
static struct fuse_operations memefs_oper = {
    .getattr = memefs_getattr,    // Required for Part 1
    .readdir = memefs_readdir,    // Required for Part 1
    .open = memefs_open,          // Required for Part 1
    .read = memefs_read,          // Required for Part 1
    .create = memefs_create,      // Required for Part 2
    .unlink = memefs_unlink,      // Required for Part 2
    .write = memefs_write,        // Required for Part 2
    .truncate = memefs_truncate,  // Optional for Part 2
    .utimens = memefs_utimens     // Optional
};

/* Cleanup resources */
static void cleanup() {
    if (dir_entries) {
        free(dir_entries);
        dir_entries = NULL;
    }
    if (img_fd != -1) {
        close(img_fd);
        img_fd = -1;
    }
}

/* Main function */
int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s [FUSE_OPTIONS] <image> <mountpoint>\n", argv[0]);
        return 1;
    }

    const char *image_file = argv[argc-2];
    const char *mountpoint = argv[argc-1];

    if (DEBUG) {
        printf("Image path: %s\n", image_file);
        printf("Mount point: %s\n", mountpoint);
    }

    // Open the image file
    img_fd = open(image_file, O_RDWR);
    if (img_fd < 0) {
        perror("Failed to open image file");
        return 1;
    }

    // Load filesystem structures
    if (load_superblock() < 0 || load_fat() < 0 || load_directory() < 0) {
        fprintf(stderr, "Failed to load filesystem structures\n");
        cleanup();
        return 1;
    }

    // Prepare FUSE arguments (skip image file path)
    char *fuse_argv[argc];
    fuse_argv[0] = argv[0];
    for (int i = 1; i < argc-2; i++) {
        fuse_argv[i] = argv[i];
    }
    fuse_argv[argc-2] = (char*)mountpoint;
    fuse_argv[argc-1] = NULL;

    // Start FUSE (page 13)
    int ret = fuse_main(argc-1, fuse_argv, &memefs_oper, NULL);
    cleanup();
    return ret;
}
