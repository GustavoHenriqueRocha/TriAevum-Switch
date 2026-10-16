#pragma once

// Host floating-point environment for native AArch64 VFP execution
// (whole_aot_cpp.py --native-vfp-aarch64).
//
// ARMv8 uses one pseudocode for AArch32 VFP and AArch64 scalar FP: the
// FPSCR control bits RMode (23:22), FZ (24) and DN (25) live at the same
// positions in FPCR, and the cumulative flags IOC, DZC, OFC, UFC, IXC (4:0)
// and IDC (7) at the same positions in FPSR. Generated code therefore runs
// with the guest's FPSCR mode in FPCR and lets the hardware accumulate the
// guest's flags in FPSR, as Dynarmic's arm64 backend does:
//
// - Oot3dGuestFpScope (entry of ExecuteOot3dWholeAotFunction) saves the host
//   FPCR/FPSR, loads FPCR from the guest FPSCR and clears FPSR. On exit it
//   ORs FPSR into the guest FPSCR and restores the host registers.
// - Oot3dGuestFpHostCallScope (external calls and block-entry callbacks)
//   folds FPSR into the guest FPSCR and runs the callee under the host FPCR;
//   afterwards it reloads FPCR from the (possibly changed) guest FPSCR.
// - Oot3dGuestFpSlowPathScope (memory slow paths) runs host code under the
//   host FPCR and restores the guest FPCR and FPSR untouched afterwards.
//
// All scopes are no-ops unless a guest scope is active on this thread, and
// compile to nothing on hosts other than AArch64. Code that never enables the
// native VFP mode pays one thread-local load per external call.

#include <cstdint>

#if defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
#define OOT3D_GUEST_FP_AARCH64 1
#else
#define OOT3D_GUEST_FP_AARCH64 0
#endif

namespace Oot3dNativeGame {

inline constexpr std::uint32_t kOot3dGuestFpControlMask = 0x03C00000U;  // DN|FZ|RMode
inline constexpr std::uint32_t kOot3dGuestFpFlagMask = 0x0000009FU;     // IDC|IXC..IOC

#if OOT3D_GUEST_FP_AARCH64

[[nodiscard]] inline std::uint64_t Oot3dReadFpcr() noexcept {
    std::uint64_t value;
    asm volatile("mrs %0, fpcr" : "=r"(value));
    return value;
}

inline void Oot3dWriteFpcr(std::uint64_t value) noexcept {
    asm volatile("msr fpcr, %0" : : "r"(value));
}

[[nodiscard]] inline std::uint64_t Oot3dReadFpsr() noexcept {
    std::uint64_t value;
    asm volatile("mrs %0, fpsr" : "=r"(value));
    return value;
}

inline void Oot3dWriteFpsr(std::uint64_t value) noexcept {
    asm volatile("msr fpsr, %0" : : "r"(value));
}

[[nodiscard]] constexpr std::uint64_t Oot3dGuestFpcr(
    std::uint64_t hostFpcr, std::uint32_t fpscr) noexcept {
    return (hostFpcr & ~std::uint64_t{kOot3dGuestFpControlMask}) |
           (fpscr & kOot3dGuestFpControlMask);
}

struct Oot3dGuestFpThreadState final {
    bool Active = false;
    std::uint64_t HostFpcr = 0;
};

inline thread_local Oot3dGuestFpThreadState gOot3dGuestFp{};

// FPSR -> guest FPSCR cumulative flags (VMRS and every observable exit).
inline void Oot3dGuestFpFoldFlags(std::uint32_t& fpscr) noexcept {
    fpscr |= static_cast<std::uint32_t>(Oot3dReadFpsr()) & kOot3dGuestFpFlagMask;
}

// VMSR: the new FPSCR replaces mode and cumulative flags.
inline void Oot3dGuestFpLoadControl(std::uint32_t fpscr) noexcept {
    Oot3dWriteFpcr(Oot3dGuestFpcr(gOot3dGuestFp.HostFpcr, fpscr));
    Oot3dWriteFpsr(0U);
}

class Oot3dGuestFpScope final {
public:
    explicit Oot3dGuestFpScope(std::uint32_t& fpscr) noexcept
        : mFpscr(fpscr),
          mPrevious(gOot3dGuestFp),
          mHostFpcr(Oot3dReadFpcr()),
          mHostFpsr(Oot3dReadFpsr()) {
        gOot3dGuestFp = {true, mHostFpcr};
        const std::uint64_t guest = Oot3dGuestFpcr(mHostFpcr, fpscr);
        if (guest != mHostFpcr) {
            Oot3dWriteFpcr(guest);
        }
        Oot3dWriteFpsr(0U);
    }

    Oot3dGuestFpScope(const Oot3dGuestFpScope&) = delete;
    Oot3dGuestFpScope& operator=(const Oot3dGuestFpScope&) = delete;

    ~Oot3dGuestFpScope() noexcept {
        Oot3dGuestFpFoldFlags(mFpscr);
        if (Oot3dReadFpcr() != mHostFpcr) {
            Oot3dWriteFpcr(mHostFpcr);
        }
        Oot3dWriteFpsr(mHostFpsr);
        gOot3dGuestFp = mPrevious;
    }

private:
    std::uint32_t& mFpscr;
    Oot3dGuestFpThreadState mPrevious;
    std::uint64_t mHostFpcr;
    std::uint64_t mHostFpsr;
};

class Oot3dGuestFpHostCallScope final {
public:
    explicit Oot3dGuestFpHostCallScope(std::uint32_t& fpscr) noexcept
        : mFpscr(fpscr), mActive(gOot3dGuestFp.Active) {
        if (mActive) [[unlikely]] {
            Oot3dGuestFpFoldFlags(fpscr);
            Oot3dWriteFpcr(gOot3dGuestFp.HostFpcr);
            Oot3dWriteFpsr(0U);
            gOot3dGuestFp.Active = false;
        }
    }

    Oot3dGuestFpHostCallScope(const Oot3dGuestFpHostCallScope&) = delete;
    Oot3dGuestFpHostCallScope& operator=(const Oot3dGuestFpHostCallScope&) = delete;

    ~Oot3dGuestFpHostCallScope() noexcept {
        if (mActive) [[unlikely]] {
            gOot3dGuestFp.Active = true;
            Oot3dGuestFpLoadControl(mFpscr);
        }
    }

private:
    std::uint32_t& mFpscr;
    bool mActive;
};

class Oot3dGuestFpSlowPathScope final {
public:
    Oot3dGuestFpSlowPathScope() noexcept : mActive(gOot3dGuestFp.Active) {
        if (mActive) [[unlikely]] {
            mGuestFpcr = Oot3dReadFpcr();
            mGuestFpsr = Oot3dReadFpsr();
            Oot3dWriteFpcr(gOot3dGuestFp.HostFpcr);
            gOot3dGuestFp.Active = false;
        }
    }

    Oot3dGuestFpSlowPathScope(const Oot3dGuestFpSlowPathScope&) = delete;
    Oot3dGuestFpSlowPathScope& operator=(const Oot3dGuestFpSlowPathScope&) = delete;

    ~Oot3dGuestFpSlowPathScope() noexcept {
        if (mActive) [[unlikely]] {
            gOot3dGuestFp.Active = true;
            Oot3dWriteFpcr(mGuestFpcr);
            Oot3dWriteFpsr(mGuestFpsr);
        }
    }

private:
    bool mActive;
    std::uint64_t mGuestFpcr = 0;
    std::uint64_t mGuestFpsr = 0;
};

#else

inline void Oot3dGuestFpFoldFlags(std::uint32_t&) noexcept {
}

inline void Oot3dGuestFpLoadControl(std::uint32_t) noexcept {
}

class Oot3dGuestFpScope final {
public:
    explicit Oot3dGuestFpScope(std::uint32_t&) noexcept {
    }
};

class Oot3dGuestFpHostCallScope final {
public:
    explicit Oot3dGuestFpHostCallScope(std::uint32_t&) noexcept {
    }
};

class Oot3dGuestFpSlowPathScope final {
public:
    Oot3dGuestFpSlowPathScope() noexcept {
    }
};

#endif

} // namespace Oot3dNativeGame
