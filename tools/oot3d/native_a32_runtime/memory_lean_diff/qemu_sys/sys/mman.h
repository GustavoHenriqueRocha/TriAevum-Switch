// <sys/mman.h> for devkitA64 test binaries under qemu-aarch64 (newlib has
// none): Linux AArch64 values; implemented in ../qemu_mman.cpp.
#pragma once

#include <stddef.h>
#include <sys/types.h>

#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_NORESERVE 0x4000
#define MAP_FAILED ((void*)-1)

#ifdef __cplusplus
extern "C" {
#endif
void* mmap(void* address, size_t length, int protection, int flags, int fd, off_t offset);
int munmap(void* address, size_t length);
#ifdef __cplusplus
}
#endif
