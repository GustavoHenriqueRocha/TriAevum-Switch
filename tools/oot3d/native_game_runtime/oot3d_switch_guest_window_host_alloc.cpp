// Host allocator hook of the guest window (oot3d_switch_guest_window.h), for
// the host test binaries only: the console's hook is in the newlib wrappers
// (oot3d_switch_guard_alloc.cpp). glibc's malloc cannot be wrapped from
// here (libstdc++.so calls it directly), so the replaceable global
// operator new/delete give the armed placement to std::vector's allocation
// and keep window buffers away from free(). Linking this file into a binary
// that already replaces them (the soak tests) would be a duplicate
// definition: it goes only into the targets listed in host_services.cmake.
#include "oot3d_switch_guest_window.h"

#include <cstdio>
#include <cstdlib>
#include <new>

namespace {

[[gnu::constructor]] void MarkHooked() {
    Oot3dNativeGame::GuestWindow::MarkAllocatorHooked();
}

// TRIAEVUM_FASTMEM_WINDOW runs of the existing tests: proof that the window
// was used, and the verify mode's result.
[[gnu::destructor]] void ReportUse() {
    const auto status = Oot3dNativeGame::GuestWindow::CurrentStatus();
    if (!status.Requested) return;
    std::fprintf(stderr,
                 "fastmem_window at exit: placed=%llu conflicts=%llu misses=%llu checks=%llu "
                 "mismatches=%llu (%s)\n",
                 static_cast<unsigned long long>(status.Placed),
                 static_cast<unsigned long long>(status.Conflicts),
                 static_cast<unsigned long long>(status.Misses),
                 static_cast<unsigned long long>(status.ShadowChecks),
                 static_cast<unsigned long long>(status.ShadowMismatches),
                 Oot3dNativeGame::GuestWindow::FormatStatus(status).c_str());
}

void* Allocate(std::size_t size) {
    if (void* placed = Oot3dNativeGame::GuestWindow::TakePlacement(size)) return placed;
    if (void* pointer = std::malloc(size == 0U ? 1U : size)) return pointer;
    throw std::bad_alloc();
}

void Free(void* pointer) noexcept {
    if (Oot3dNativeGame::GuestWindow::Contains(pointer)) {
        Oot3dNativeGame::GuestWindow::ReleaseAllocation(pointer);
        return;
    }
    std::free(pointer);
}

} // namespace

void* operator new(std::size_t size) { return Allocate(size); }
void* operator new[](std::size_t size) { return Allocate(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return Allocate(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
    return operator new(size, tag);
}
void operator delete(void* pointer) noexcept { Free(pointer); }
void operator delete[](void* pointer) noexcept { Free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { Free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { Free(pointer); }
void operator delete(void* pointer, const std::nothrow_t&) noexcept { Free(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { Free(pointer); }
