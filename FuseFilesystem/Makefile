CC = gcc
CFLAGS = -Wall -Wextra -D_FILE_OFFSET_BITS=64
FUSE_CFLAGS = $(shell pkg-config --cflags fuse3)
FUSE_LIBS = $(shell pkg-config --libs fuse3)

all: mkmemefs memefs

mkmemefs: mkmemefs.c
	$(CC) $(CFLAGS) -o $@ $<

memefs: memefs.c
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -o $@ $< $(FUSE_LIBS) -pthread

clean:
	rm -f mkmemefs memefs *.img

.PHONY: all clean
