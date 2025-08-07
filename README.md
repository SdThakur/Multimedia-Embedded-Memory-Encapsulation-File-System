                                                       **Project 4 - Multimedia Embedded Memory Encapsulation File System**

                                              Contact Name : Satya Thakur, Student Id : UW61778, Email address : UW61778@UMBC.EDU

**Overview**: This project implements a custom filesystem called MEMEFS (Multimedia Embedded Memory Encapsulation Filesystem) using FUSE (Filesystem in Userspace). The implementation includes:
  -A read-only filesystem (Part 1)
  -A read-write filesystem with full file operations (Part 2)

**The filesystem uses a disk image format with:**

  -Superblocks (main and backup)
  -File Allocation Table (FAT)
  -Directory structure
  -Data blocks

**Features **

Part 1: Read-Only Operations getattr: Get file attributes

  -readdir: List directory contents
  -open: Open files
  -read: Read file contents

Part 2: Write Operations create: Create new files

  -unlink: Delete files
  -write: Write to files
  -truncate: Change file size

**Build Instructions Prerequisites Linux system**
  -FUSE3 libraries

**GCC compiler**
  -sudo apt-get install fuse3 libfuse3-3 libfuse3-dev gcc make

**Compilation** 
  -make

**This will build:**

  -mkmemefs: Tool to create MEMEFS disk images
  -memefs: The FUSE filesystem implementation

**Usage**

Create a filesystem image ./mkmemefs test.img This creates a new MEMEFS image with a sample file "HELLO.TXT".

Mount the filesystem mkdir -p /tmp/memefs ./memefs test.img /tmp/memefs -f -d The -f flag keeps it in foreground, -d enables debug output.

Basic Operations Read-only operations: ls -la /tmp/memefs cat /tmp/memefs/HELLO.TXT

Write operations:

Create new file
echo "Test content" > /tmp/memefs/TEST.TXT

Append to file
echo "More content" >> /tmp/memefs/TEST.TXT

Delete file
rm /tmp/memefs/HELLO.TXT

Unmount the filesystem: fusermount3 -u /tmp/memefs

`Complete Test Workflow:`
# 1. Build both programs
make

# 2. Create image (in current directory)
./mkmemefs test.img

# 3. Create mount point
mkdir test_mount

# 4. Mount with debug output
./memefs -f -d test.img test_mount

# 5. In another terminal, test the mount:
ls -l test_mount
cat test_mount/HELLO.TXT

# 6. When done, unmount
fusermount -u test_mount
