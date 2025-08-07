/*
 * mkmemefs.c - MEMEFS filesystem image creator
 * 
 * Creates a disk image conforming to MEMEFS specifications for CMSC 421 Project 4
 * 
 * - Block 0: Backup superblock
 * - Block 1-220: User data blocks
 * - Block 239: Backup FAT
 * - Block 253-254: Directory blocks
 * - Block 255: Primary superblock
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>  // For htons/htonl
#include <ctype.h>
#include <errno.h>

// Filesystem constants
#define BLOCK_SIZE 512
#define TOTAL_BLOCKS 256
#define USER_BLOCKS 220
#define FAT_BLOCKS 1
#define DIR_BLOCKS 14
#define MAX_FILENAME 8
#define MAX_EXTENSION 3
#define SIGNATURE "?MEMEFS++CMSC421"
#define VOLUME_LABEL "MEMEFS_VOLUME"
#define FILE_PERMISSIONS 0x81FF  // Regular file with full permissions (rwxrwxrwx)

// Superblock structure
typedef struct {
    char signature[16];      // Filesystem signature
    uint8_t mounted;         // Mount status flag
    uint8_t reserved[3];     // Padding
    uint32_t version;        // Filesystem version
    uint8_t creation_time[8]; // BCD timestamp
    uint16_t main_fat_block; // Main FAT location
    uint16_t main_fat_size;  // Main FAT size in blocks
    uint16_t backup_fat_block; // Backup FAT location
    uint16_t backup_fat_size; // Backup FAT size
    uint16_t dir_start_block; // Directory start block
    uint16_t dir_size;       // Directory size in blocks
    uint16_t user_blocks;    // Number of user blocks
    uint16_t first_user_block; // First user block
    char volume_label[16];   // Volume name
    uint8_t unused[448];     // Padding to fill block
} Superblock;

// Directory entry structure 
typedef struct {
    uint16_t type_perms;     // File type and permissions
    uint16_t start_block;    // First block of file
    char filename[11];       // 8.3 filename (space padded)
    uint8_t unused;          // Padding
    uint8_t timestamp[8];    // BCD modification time
    uint32_t file_size;      // File size in bytes
    uint16_t owner_uid;      // Owner user ID
    uint16_t group_gid;      // Owner group ID
} DirEntry;

// Error handling macros
#define CHECK_SYS(call, msg) if ((call) == -1) { perror(msg); goto error; }
#define CHECK_WRITE(fd, buf, size, msg) \
    if (write(fd, buf, size) != (ssize_t)(size)) { perror(msg); goto error; }

/* Convert values to big-endian as specified on page 3 */
static uint16_t to_be16(uint16_t val) { return htons(val); }
static uint32_t to_be32(uint32_t val) { return htonl(val); }

/* Convert number to BCD format */
static uint8_t to_bcd(uint8_t num) {
    if (num > 99) return 0xFF;
    return ((num / 10) << 4) | (num % 10);
}

/* Generate BCD timestamp */
static void generate_timestamp(uint8_t bcd_time[8]) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    
    bcd_time[0] = to_bcd(20); // Hardcoded 21st century
    bcd_time[1] = to_bcd(tm->tm_year % 100);
    bcd_time[2] = to_bcd(tm->tm_mon + 1);
    bcd_time[3] = to_bcd(tm->tm_mday);
    bcd_time[4] = to_bcd(tm->tm_hour);
    bcd_time[5] = to_bcd(tm->tm_min);
    bcd_time[6] = to_bcd(tm->tm_sec);
    bcd_time[7] = 0; // Reserved
}

/* Validate 8.3 filename format */
static int validate_filename(const char *name, const char *ext) {
    if (!name || !ext) return 0;
    
    // Check lengths
    size_t name_len = strlen(name);
    if (name_len == 0 || name_len > MAX_FILENAME) return 0;
    if (strlen(ext) > MAX_EXTENSION) return 0;
    
    // Check valid characters 
    for (size_t i = 0; i < name_len; i++) {
        if (!isalnum(name[i]) && 
            name[i] != '^' && name[i] != '~' && name[i] != '_' &&
            name[i] != '=' && name[i] != '|') {
            return 0;
        }
    }
    return 1;
}

/* Write zero-filled blocks to image */
static int write_empty_blocks(int fd, int count) {
    uint8_t block[BLOCK_SIZE] = {0};
    for (int i = 0; i < count; i++) {
        CHECK_WRITE(fd, block, BLOCK_SIZE, "Failed to write empty block");
    }
    return 0;

error:
    return -1;
}

/* Create the sample file HELLO.TXT */
static int create_sample_file(int fd, const char *name, const char *ext, 
                           const char *content, uint16_t start_block) {
    size_t size = strlen(content);
    
    // Write file content to block 1
    CHECK_SYS(lseek(fd, start_block * BLOCK_SIZE, SEEK_SET), "Seek to file block");
    CHECK_WRITE(fd, content, size, "Write file content");
    
    // Prepare directory entry 
    DirEntry entry = {0};
    entry.type_perms = to_be16(FILE_PERMISSIONS);
    entry.start_block = to_be16(start_block);
    
    // Format 8.3 filename 
    memset(entry.filename, ' ', 11);
    strncpy(entry.filename, name, MAX_FILENAME);
    strncpy(entry.filename + MAX_FILENAME, ext, MAX_EXTENSION);
    
    // Set metadata
    generate_timestamp(entry.timestamp);
    entry.file_size = to_be32(size);
    entry.owner_uid = to_be16(getuid());
    entry.group_gid = to_be16(getgid());
    
    // Write to first directory entry (block 253)
    CHECK_SYS(lseek(fd, 253 * BLOCK_SIZE, SEEK_SET), "Seek to directory");
    CHECK_WRITE(fd, &entry, sizeof(entry), "Write directory entry");
    
    return 0;

error:
    return -1;
}

/* Write FAT to specified block */
static int write_fat(int fd, uint16_t fat_block, uint16_t *fat) {
    // Convert to big-endian for storage
    uint16_t fat_be[TOTAL_BLOCKS];
    for (int i = 0; i < TOTAL_BLOCKS; i++) {
        fat_be[i] = to_be16(fat[i]);
    }
    
    CHECK_SYS(lseek(fd, fat_block * BLOCK_SIZE, SEEK_SET), "Seek to FAT");
    CHECK_WRITE(fd, fat_be, sizeof(fat_be), "Write FAT");
    return 0;

error:
    return -1;
}

/* Write superblock to specified block */
static int write_superblock(int fd, uint16_t block_num, const Superblock *sb) {
    CHECK_SYS(lseek(fd, block_num * BLOCK_SIZE, SEEK_SET), "Seek to superblock");
    CHECK_WRITE(fd, sb, sizeof(Superblock), "Write superblock");
    return 0;

error:
    return -1;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <output_file.img>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int fd = -1;
    int ret = EXIT_FAILURE;
    uint16_t fat[TOTAL_BLOCKS] = {0};  // Initialize all blocks as free
    
    // Sample file data (page 20)
    const char *filename = "HELLO";
    const char *extension = "TXT";
    const char *content = "Hello from MEMEFS!\n";
    
    if (!validate_filename(filename, extension)) {
        fprintf(stderr, "Invalid filename/extension\n");
        return EXIT_FAILURE;
    }

    // Create image file
    fd = open(argv[1], O_RDWR | O_CREAT | O_TRUNC, 0666);
    CHECK_SYS(fd, "Failed to Create image file");

    // Step 1: Initialize all blocks to zero
    if (write_empty_blocks(fd, TOTAL_BLOCKS) < 0) {
        goto cleanup;
    }

    // Step 2: Set up FAT
    fat[1] = 0xFFFF;  // Mark block 1 as used (HELLO.TXT)
    
    // Write main FAT (block 254)
    if (write_fat(fd, 254, fat) < 0) {
        goto cleanup;
    }
    
    // Write backup FAT (block 239)
    if (write_fat(fd, 239, fat) < 0) {
        goto cleanup;
    }

    // Step 3: Create sample file
    if (create_sample_file(fd, filename, extension, content, 1) < 0) {
        goto cleanup;
    }

    // Step 4: Initialize superblock
    Superblock sb = {0};
    memcpy(sb.signature, SIGNATURE, 16);
    sb.version = to_be32(0x00000001);
    generate_timestamp(sb.creation_time);
    
    // Filesystem layout info
    sb.main_fat_block = to_be16(254);
    sb.main_fat_size = to_be16(FAT_BLOCKS);
    sb.backup_fat_block = to_be16(239);
    sb.backup_fat_size = to_be16(FAT_BLOCKS);
    sb.dir_start_block = to_be16(253);
    sb.dir_size = to_be16(DIR_BLOCKS);
    sb.user_blocks = to_be16(USER_BLOCKS);
    sb.first_user_block = to_be16(1);
    
    // Volume info
    strncpy(sb.volume_label, VOLUME_LABEL, sizeof(sb.volume_label) - 1);
    sb.volume_label[sizeof(sb.volume_label) - 1] = '\0';

    // Write primary superblock (block 255)
    if (write_superblock(fd, 255, &sb) < 0) {
        goto cleanup;
    }
    
    // Write backup superblock (block 0)
    if (write_superblock(fd, 0, &sb) < 0) {
        goto cleanup;
    }

    printf("MEMEFS image created successfully: %s\n", argv[1]);
    ret = EXIT_SUCCESS;

error:  // Added this label for CHECK_SYS macro
cleanup:
    if (fd != -1) {
        close(fd);
        if (ret != EXIT_SUCCESS) {
            unlink(argv[1]); // Remove failed image
        }
    }
    return ret;
}
