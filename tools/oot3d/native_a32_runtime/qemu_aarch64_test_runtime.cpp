// Minimal Linux/AArch64 user-mode runtime that lets devkitA64 (newlib,
// aarch64-none-elf) build the VFP differential tests as static executables
// for qemu-aarch64: _start, static constructors, a static heap, printf to
// stdout and exit. Link with -static -nostartfiles. See
// run_vfp_aarch64_tests_qemu.sh.

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>

extern "C" {

static long LinuxSyscall3(long number, long a, long b, long c) {
    register long x8 asm("x8") = number;
    register long x0 asm("x0") = a;
    register long x1 asm("x1") = b;
    register long x2 asm("x2") = c;
    asm volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}

static void WriteStdout(const char* text, std::size_t size) {
    LinuxSyscall3(64 /* write */, 1, reinterpret_cast<long>(text),
                  static_cast<long>(size));
}

[[noreturn]] static void ExitProcess(int code) {
    LinuxSyscall3(94 /* exit_group */, code, 0, 0);
    for (;;) {
    }
}

int main();

extern void (*__init_array_start[])(void) __attribute__((weak));
extern void (*__init_array_end[])(void) __attribute__((weak));
// devkitPro's libsysbase serves sbrk from this range.
extern char* fake_heap_start;
extern char* fake_heap_end;

alignas(16) static unsigned char gHeap[256U << 20U];

void Oot3dQemuTestStart() {
    fake_heap_start = reinterpret_cast<char*>(gHeap);
    fake_heap_end = reinterpret_cast<char*>(gHeap) + sizeof(gHeap);
    for (auto entry = __init_array_start; entry < __init_array_end; ++entry) {
        (*entry)();
    }
    ExitProcess(main());
}

// qemu-user enters with a valid stack pointer.
void _start() {
    asm volatile("mov x29, #0\n\tbl Oot3dQemuTestStart");
}

int printf(const char* format, ...) {
    char buffer[1024];
    va_list arguments;
    va_start(arguments, format);
    const int size = vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    WriteStdout(buffer, std::strlen(buffer));
    return size;
}

} // extern "C"
