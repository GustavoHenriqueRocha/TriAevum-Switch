// mmap/munmap for devkitA64 test binaries under qemu-aarch64 (Linux user mode):
// the fastmem window's host backend (oot3d_switch_guest_window.cpp built with
// OOT3D_GUEST_WINDOW_MMAP) in run_store_sequence_diff.sh TARGET=switch.
// Declarations in qemu_sys/sys/mman.h; Linux AArch64 system call numbers.
#include <sys/mman.h>

#include <cstddef>

extern "C" {

static long QemuSyscall6(long number, long a, long b, long c, long d, long e, long f) {
    register long x8 asm("x8") = number;
    register long x0 asm("x0") = a;
    register long x1 asm("x1") = b;
    register long x2 asm("x2") = c;
    register long x3 asm("x3") = d;
    register long x4 asm("x4") = e;
    register long x5 asm("x5") = f;
    asm volatile("svc #0"
                 : "+r"(x0)
                 : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                 : "memory");
    return x0;
}

void* mmap(void* address, size_t length, int protection, int flags, int fd, off_t offset) {
    const long result = QemuSyscall6(222 /* mmap */, reinterpret_cast<long>(address),
                                     static_cast<long>(length), protection, flags, fd,
                                     static_cast<long>(offset));
    return result < 0 && result > -4096 ? MAP_FAILED : reinterpret_cast<void*>(result);
}

int munmap(void* address, size_t length) {
    return QemuSyscall6(215 /* munmap */, reinterpret_cast<long>(address),
                        static_cast<long>(length), 0, 0, 0, 0) == 0
               ? 0
               : -1;
}

} // extern "C"
