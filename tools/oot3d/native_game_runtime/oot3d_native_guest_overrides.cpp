// Native replacements for the hottest guest functions. Contract and guarantees
// in oot3d_native_guest_overrides.h; each function below mirrors the generated
// translation (same memory access order, same VFP calls, same flag masks),
// only without block-by-block entry, without the per-instruction FPSCR test,
// and with registers in local variables.
//
// Rules followed by all of them:
//  1. Every decline (return Decline(...)) happens before the first side
//     effect: no write to guest memory, state, or context.
//  2. Before the first effect, every later access is validated to hit the
//     memory fast path (fast page, aligned, not crossing a page); the
//     accesses are then made with the SAME ReadFast/WriteFast calls as the
//     translation, in the same order, so write generations, page stamps and
//     diagnostic counters stay identical.
//  3. VLDR literals in the text segment (read-only, same as the generator's
//     literal folding) become constants.
//  4. VFP operations use the same helpers as the generated code
//     (namespace Vfp below: on AArch64 the Oot3dVfpZeroAware*/Native* of
//     --native-vfp-aarch64, on other hosts the Oot3dVfp* fast paths), with
//     FPSCR evolving in the same order; no host floating-point arithmetic is
//     used directly.

#include "oot3d_native_guest_overrides.h"

#include "oot3d_native_a32_memory.h"
#include "oot3d_native_mtx3x4_kernel.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace Oot3dNativeGame {

bool gOot3dGuestOverridesEnabled = true;
Oot3dGuestOverrideProfileHook gOot3dGuestOverrideProfileHook{};

void SetOot3dGuestOverridesEnabled(bool enabled) noexcept {
    gOot3dGuestOverridesEnabled = enabled;
}

namespace {

namespace a32 = oot3d::recomp::a32;

enum OverrideIndex : size_t {
    kIdx00372070,
    kIdx002CDE90,
    kIdx003094CC,
    kIdx002F9430,
    kIdx0036E168,
    kIdx003325F0,
    kIdx00307C94,
    kIdx003727F0,
    kIdx004A022C,
    kIdx004A0338,
    kIdx00371738,
    kIdx00466E68,
    kIdx003247B0,
    kIdx0047AF24,
    kIdx002FA674,
    kIdx00307B50,
    kIdx00307DC8,
    kIdx00308920,
    kIdx00308604,
    kIdx002BBB14,
    kIdx00303C78,
    kIdx0046700C,
    kIdx003563E0,
    kIdx002DCEF4,
    kOverrideCount,
};

Oot3dGuestOverrideInfo gOverrides[kOverrideCount] = {
    {0x00372070U, "oot3d_translate_mtx3x4_local"},
    {0x002CDE90U, "FUN_002cde90"},
    {0x003094CCU, "FUN_003094cc"},
    {0x002F9430U, "PauseRenderBuffer_SetVec2Range"},
    {0x0036E168U, "Math_SmoothStepToF"},
    {0x003325F0U, "Math3D_CylOutsideCylDist"},
    {0x00307C94U, "PicaCommandWriter_UploadVertexFloatUniforms"},
    {0x003727F0U, "Math_SinF"},
    {0x004A022CU, "AudioEffect_ProcessFourChannelDelay"},
    {0x004A0338U, "AudioEffect_ProcessStereoReverb"},
    {0x00371738U, "Runtime_Memcpy"},
    {0x00466E68U, "MeshCommandPacket_Submit:copy"},
    {0x003247B0U, "BgCheck_LineTestImpl:clear_marks"},
    {0x0047AF24U, "FUN_0047af24"},
    {0x002FA674U, "CalculateGyroscopeAxisStatus:mean"},
    {0x00307B50U, "PicaTev_ConvertSource"},
    {0x00307DC8U, "PicaCompare_ConvertFunction"},
    {0x00308920U, "NativeCurve_SampleFloat:search"},
    {0x00308604U, "CsabCurve_SampleRotation:search"},
    {0x002BBB14U, "Math3D_CirSquareVsTriSquare"},
    {0x00303C78U, "FUN_00303b24:copy"},
    {0x0046700CU, "MeshCommandPacket_PatchBoneUniforms:copy"},
    {0x003563E0U, "FUN_003562c0:walk"},
    {0x002DCEF4U, "Mtx3x3_Multiply"},
};

constexpr uint32_t kFpscrTrapMask = 0x00379F00U; // LEN/STRIDE and exception enables
constexpr uint32_t kPageSize = 0x1000U;

inline bool Decline(size_t index) noexcept {
    ++gOverrides[index].Declined;
    return false;
}

inline bool Accept(size_t index) noexcept {
    ++gOverrides[index].Calls;
    return true;
}

// Does any block-entry hook (or the "all blocks" mode) cover
// [first, last]? Same as NativeA32UnobservedMemory::Observed, except for the
// all-blocks guest profile callback: it only observes what it forwards to the
// original callback (see Oot3dGuestOverrideProfileHook). `first` is always
// the replacement's entry.
inline bool Hooked(const Oot3dWholeAotContext& context, uint32_t first,
                   uint32_t last) noexcept {
    if (context.BlockEntry == nullptr) return false;
    if (context.BlockEntryPcCount == 0U) {
        const auto& profile = gOot3dGuestOverrideProfileHook;
        if (context.BlockEntry != profile.Callback || profile.Callback == nullptr) {
            return true;
        }
        if (profile.Forwards(context.BlockEntryUser, first, last)) return true;
        profile.Mark(context.BlockEntryUser, first);
        return false;
    }
    const uint32_t* end = context.BlockEntryPcs + context.BlockEntryPcCount;
    const uint32_t* hook = std::lower_bound(context.BlockEntryPcs, end, first);
    return hook != end && *hook <= last;
}

inline bool Budget(const Oot3dWholeAotContext& context,
                   uint32_t blocks) noexcept {
    return context.BlocksRemaining >= blocks;
}

inline void Consume(Oot3dWholeAotContext& context, uint32_t blocks) noexcept {
    context.BlocksRemaining -= blocks;
}

inline bool VfpDefault(uint32_t fpscr) noexcept {
    return (fpscr & kFpscrTrapMask) == 0U;
}

// [address, address + size) aligned to `align`, not wrapping around the
// 32-bit space and covered by fast pages (writable ones, if `write`).
// Guarantees ReadFast/WriteFast of size <= align in that range do not fail.
bool FastRange(const NativeA32Memory& memory, uint32_t address, uint32_t size,
               uint32_t align, bool write) noexcept {
    if (size == 0U) return true;
    if ((address & (align - 1U)) != 0U) return false;
    if (static_cast<uint64_t>(address) + size > (uint64_t{1} << 32U)) {
        return false;
    }
    while (size != 0U) {
        const uint32_t room = kPageSize - (address & (kPageSize - 1U));
        const uint32_t chunk = std::min(room, size);
        const bool fast = write ? memory.SpanOnFastWritePage(address, chunk)
                                : memory.SpanOnFastReadPage(address, chunk);
        if (!fast) return false;
        address += chunk;
        size -= chunk;
    }
    return true;
}

inline bool Readable(const NativeA32Memory& memory, uint32_t address,
                     uint32_t size, uint32_t align = 4U) noexcept {
    return FastRange(memory, address, size, align, false);
}

inline bool Writable(const NativeA32Memory& memory, uint32_t address,
                     uint32_t size, uint32_t align = 4U) noexcept {
    return FastRange(memory, address, size, align, true);
}

// Accumulates the result of a VFP operation like Oot3dAotCommitVfp.
inline uint32_t Commit(uint32_t& fpscr, a32::VfpBinary32Result result) noexcept {
    fpscr |= result.exception_flags;
    return result.value;
}

// VCMP/VCMPE followed by VMRS APSR_nzcv: NZCV from FPSCR and flags. In
// software (locals only, nothing in FPSR): also usable before a decline.
inline void Compare(uint32_t& fpscr, Oot3dAotLazyFlags& flags, uint32_t left,
                    uint32_t right, bool signaling) noexcept {
    const auto comparison = a32::VfpBinary32Compare(left, right, signaling);
    fpscr = (fpscr & ~0xF0000000U) | (comparison.value & 0xF0000000U);
    fpscr |= comparison.exception_flags;
    flags.AssignPackedNzcv(fpscr);
}

// VFP arithmetic for the replacements: the same calls as the generated code.
//
// On AArch64 (Switch, qemu-aarch64) the replacements run inside
// Oot3dGuestFpScope: FPCR with the guest FPSCR mode and FPSR folded into the
// guest FPSCR on exit (and on every VMRS). There only the
// Oot3dVfpZeroAware*/Native* are valid (oot3d_native_a32_vfp_aarch64.h, those
// of the C++ generated with --native-vfp-aarch64 --vfp-zero-aware), which run
// the guest instruction (FZ=1, one instruction on the hot path) or the
// software path, which is integer arithmetic only. The fast paths of
// oot3d_native_a32_vfp_fast.h DO NOT work there: their AUXILIARY computations
// (the sum's TwoSum, the exact double product converted to float) raise
// FPSR flags that the result does not raise, and these leak into the
// guest:
//   - TwoSum: `sum - a` overflows (OFC, and IOC from inf - inf) when the
//     right operand is near ±FLT_MAX and the sum rounds outward
//     (Math_SmoothStepToF, attempt 140844);
//   - product: the double -> float conversion detects tininess BEFORE
//     rounding (ARMv8) and raises UFC on a product that rounds to
//     ±2^-126, where the software model only raises IXC (FZ=0, RN:
//     oot3d_translate_mtx3x4_local with 2^-24 * -0x8C7FFFFF).
// On other hosts there is no guest FP scope (host flags never reach the
// FPSCR) and the generated code uses the fast paths: they remain here.
namespace Vfp {
#if OOT3D_GUEST_FP_AARCH64
inline void Add(uint32_t& d, uint32_t l, uint32_t r, uint32_t& fpscr) noexcept {
    Oot3dVfpZeroAwareAdd(d, l, r, fpscr);
}
inline void Subtract(uint32_t& d, uint32_t l, uint32_t r, uint32_t& fpscr) noexcept {
    Oot3dVfpZeroAwareSubtract(d, l, r, fpscr);
}
inline void Multiply(uint32_t& d, uint32_t l, uint32_t r, uint32_t& fpscr) noexcept {
    Oot3dVfpZeroAwareMultiply(d, l, r, fpscr);
}
// VMLA: d = acc + l * r.
inline void MultiplyAccumulate(uint32_t& d, uint32_t acc, uint32_t l, uint32_t r,
                               uint32_t& fpscr) noexcept {
    Oot3dVfpZeroAwareMultiplyAccumulate(d, acc, l, r, fpscr);
}
// VMLS: d = acc - l * r.
inline void MultiplySubtract(uint32_t& d, uint32_t acc, uint32_t l, uint32_t r,
                             uint32_t& fpscr) noexcept {
    Oot3dVfpZeroAwareMultiplySubtract(d, acc, l, r, fpscr);
}
inline void SquareRoot(uint32_t& d, uint32_t value, uint32_t& fpscr) noexcept {
    Oot3dVfpZeroAwareSquareRoot(d, value, fpscr);
}
#else
inline void Add(uint32_t& d, uint32_t l, uint32_t r, uint32_t& fpscr) noexcept {
    d = Commit(fpscr, Oot3dVfpAdd(l, r, fpscr));
}
inline void Subtract(uint32_t& d, uint32_t l, uint32_t r, uint32_t& fpscr) noexcept {
    d = Commit(fpscr, Oot3dVfpSubtract(l, r, fpscr));
}
inline void Multiply(uint32_t& d, uint32_t l, uint32_t r, uint32_t& fpscr) noexcept {
    d = Commit(fpscr, Oot3dVfpMultiply(l, r, fpscr));
}
inline void MultiplyAccumulate(uint32_t& d, uint32_t acc, uint32_t l, uint32_t r,
                               uint32_t& fpscr) noexcept {
    d = Commit(fpscr, Oot3dVfpMultiplyAccumulate(acc, l, r, fpscr));
}
inline void MultiplySubtract(uint32_t& d, uint32_t acc, uint32_t l, uint32_t r,
                             uint32_t& fpscr) noexcept {
    d = Commit(fpscr, Oot3dVfpMultiplySubtract(acc, l, r, fpscr));
}
inline void SquareRoot(uint32_t& d, uint32_t value, uint32_t& fpscr) noexcept {
    d = Commit(fpscr, a32::VfpBinary32SquareRoot(value, fpscr));
}
#endif
// VCVT.F32.S32 and VCVT.S32.F32 (native on AArch64, software elsewhere).
inline void FromSigned(uint32_t& d, uint32_t value, uint32_t& fpscr) noexcept {
#if OOT3D_GUEST_FP_AARCH64
    Oot3dVfpNativeFromSigned(d, value, fpscr);
#else
    d = Commit(fpscr, Oot3dVfpFromSigned(value, fpscr));
#endif
}
inline void ToSigned(uint32_t& d, uint32_t value, uint32_t& fpscr) noexcept {
    Oot3dVfpNativeToSigned(d, value, fpscr);
}
} // namespace Vfp

inline uint32_t Read32(NativeA32Memory& memory, uint32_t address) noexcept {
    uint32_t value = 0U;
    memory.ReadFast<uint32_t>(address, &value);
    return value;
}

// [a, a + alen) and [b, b + blen) overlap (64-bit arithmetic).
inline bool Overlaps(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen) noexcept {
    return uint64_t{a} < uint64_t{b} + blen && uint64_t{b} < uint64_t{a} + alen;
}

// Read of `size` bytes that fits in a single fast page (exact condition of
// the ReadFast<T> fast path for T of `size` bytes).
inline bool SinglePageReadable(const NativeA32Memory& memory, uint32_t address,
                               uint32_t size) noexcept {
    return memory.SpanOnFastReadPage(address, size);
}

// Guest registers in local variables, with exact exit on the block budget
// limit: Enter() does what Oot3dAotEnterBlock would do in a hookless block;
// when the budget runs out, BlockLimit() writes the state as the translation
// would leave it at that point (registers and lazy flags) and returns the
// same exit flow.
struct LocalGuest {
    Oot3dWholeAotContext& Context;
    Oot3dAotArchitecturalState& State;
    std::array<uint32_t, 15> R;
    Oot3dAotLazyFlags Flags;

    LocalGuest(Oot3dWholeAotContext& context, Oot3dAotArchitecturalState& state) noexcept
        : Context(context), State(state), R(state.R), Flags(state.Flags) {}

    bool Enter() noexcept {
        if (Context.BlocksRemaining == 0U) return false;
        --Context.BlocksRemaining;
        return true;
    }
    void Flush() noexcept {
        State.R = R;
        State.Flags = Flags;
    }
    Oot3dWholeAotFlow BlockLimit(uint32_t pc) noexcept {
        Flush();
        return Oot3dAotBlockLimit(Context, pc);
    }
};

// Access to the NativeA32Memory fast page table without widening the
// header's public interface. Uses the standard rule that explicit
// instantiations may name private members. The constants below mirror those
// in oot3d_native_a32_memory.h and FastPageOf/ResolveCursor reproduce
// FastPageFor and the page-table test of the slow paths (a fast entry points
// at the same bytes as the window); the writes through these pointers then
// take WriteFast's bookkeeping with NativeA32Memory::NoteWrite (the cards of
// the written pages), so the end state is the translation's whatever the
// store order.
template <class Tag, auto Member> struct ExposeMember {
    friend constexpr auto MemberOf(Tag) noexcept { return Member; }
};
struct FastGroupsTag { friend constexpr auto MemberOf(FastGroupsTag) noexcept; };
struct TraceTag { friend constexpr auto MemberOf(TraceTag) noexcept; };
template struct ExposeMember<FastGroupsTag, &NativeA32Memory::mFastPageGroups>;
template struct ExposeMember<TraceTag, &NativeA32Memory::mTraceWriteFingerprint>;

constexpr uint32_t kMemPageShift = 12U;     // NativeA32Memory::PageShift
constexpr uint32_t kMemGroupShift = 20U;    // NativeA32Memory::FastGroupShift
constexpr uint32_t kMemGroupPageMask = 0xFFU;
constexpr uint32_t kMemPageMask = 0xFFFU;
constexpr uint32_t kMemPageSize = 0x1000U;

// Fast page of [address, address + size) (null if it crosses a page).
inline auto* FastPageOf(NativeA32Memory& memory, uint32_t address, uint32_t size) noexcept {
    auto& groups = memory.*MemberOf(FastGroupsTag{});
    using Page = std::remove_reference_t<decltype(groups[0]->Pages[0])>;
    const uint32_t offset = address & kMemPageMask;
    if (size == 0U || size > kMemPageSize - offset) return static_cast<Page*>(nullptr);
    auto& group = groups[address >> kMemGroupShift];
    return group ? &group->Pages[(address >> kMemPageShift) & kMemGroupPageMask]
                 : static_cast<Page*>(nullptr);
}

// Run of guest words inside a read-write fast page (the audio loops): up to
// `Words` words starting at Bytes (guest address Address). The loops access
// the words through this pointer and do the WriteFast bookkeeping at the end
// of each run (CommitSamples).
struct Cursor {
    uint8_t* Bytes = nullptr;
    uint32_t Address = 0U;
    uint32_t Words = 0U;
};

// Up to `words` words starting at `address` (4-aligned), clipped at the end
// of the page. False if the page is not fast for read and write through the
// same pointer, or if the write is tracked (fingerprint: then each WriteFast
// counts and the loop takes the per-word path).
inline bool ResolveCursor(NativeA32Memory& memory, uint32_t address, uint32_t words,
                          Cursor& cursor) noexcept {
    if (memory.*MemberOf(TraceTag{}) || words == 0U || (address & 3U) != 0U) return false;
    const uint32_t room = (kMemPageSize - (address & kMemPageMask)) >> 2U;
    const uint32_t take = std::min(room, words);
    auto* page = FastPageOf(memory, address, 4U * take);
    if (page == nullptr || page->Write == nullptr || page->Bytes != page->Write ||
        page->Read != page->Write) {
        return false;
    }
    cursor = {page->Write + (address & kMemPageMask), address, take};
    return true;
}

inline uint32_t CursorLoad(const Cursor& cursor, uint32_t index) noexcept {
    return LoadGuestValue<uint32_t>(cursor.Bytes + 4U * index);
}

inline void CursorStore(const Cursor& cursor, uint32_t index, uint32_t value) noexcept {
    StoreGuestValue<uint32_t>(cursor.Bytes + 4U * index, value);
}

// Bookkeeping for a run of samples (at least one) in which every cursor was
// written: the card of each cursor's page, as the translation's WriteFast
// calls leave it.
inline void CommitSamples(NativeA32Memory& memory,
                          std::initializer_list<const Cursor*> written) noexcept {
    for (const Cursor* cursor : written) memory.NoteWrite(cursor->Address, 4U);
}

inline Oot3dAotFlagMask Mask(uint8_t bits) noexcept {
    return static_cast<Oot3dAotFlagMask>(bits);
}

// Copy of `bytes` from `from` to `to` like the translation's ascending
// sequence of `unit`-byte WriteFast (each read before the write to the same
// address; with no overlap, the order between reads and writes does not
// matter): the same bytes and the same written pages. Preconditions:
// Readable(from, bytes, unit), Writable(to, bytes, unit), disjoint ranges.
void CopyRun(NativeA32Memory& memory, uint32_t to, uint32_t from, uint32_t bytes) noexcept {
    uint32_t done = 0U;
    while (done < bytes) {
        const uint32_t source = from + done, target = to + done;
        const uint32_t chunk = std::min({bytes - done, kMemPageSize - (source & kMemPageMask),
                                         kMemPageSize - (target & kMemPageMask)});
        auto* readPage = FastPageOf(memory, source, chunk);
        auto* writePage = FastPageOf(memory, target, chunk);
        std::memcpy(writePage->Write + (target & kMemPageMask),
                    readPage->Bytes + (source & kMemPageMask), chunk);
        memory.NoteWrite(target, chunk);
        done += chunk;
    }
}

// Words of an STM/PUSH like the generated code: WriteWordsFast when all of
// them fit in one fast page, otherwise word by word (same final effect).
// Precondition: Writable(address, 4 * count).
inline void StoreWords(NativeA32Memory& memory, uint32_t address, const uint32_t* words,
                       uint32_t count) noexcept {
    if (!memory.WriteWordsFast(address, words, count)) {
        for (uint32_t i = 0; i < count; ++i) memory.WriteFast<uint32_t>(address + 4U * i, words[i]);
    }
}

// Write of `size` bytes that hits the WriteFast<T> fast path.
inline bool SinglePageWritable(const NativeA32Memory& memory, uint32_t address,
                               uint32_t size) noexcept {
    return memory.SpanOnFastWritePage(address, size);
}

// `writes` consecutive writes of the same word (in-memory counter in a loop):
// the last value remains and the page is marked written once at least one
// write happened. Precondition: SinglePageWritable(address, 4).
inline void RepeatStore32(NativeA32Memory& memory, uint32_t address, uint32_t value,
                          uint32_t writes) noexcept {
    if (writes == 0U) return;
    auto* page = FastPageOf(memory, address, 4U);
    std::memcpy(page->Write + (address & kMemPageMask), &value, 4U);
    memory.NoteWrite(address, 4U);
}

// Same for `bytes` single-byte writes of the same value (STRB in a loop).
void FillBytes(NativeA32Memory& memory, uint32_t to, uint8_t value, uint32_t bytes) noexcept {
    uint32_t done = 0U;
    while (done < bytes) {
        const uint32_t target = to + done;
        const uint32_t chunk = std::min(bytes - done, kMemPageSize - (target & kMemPageMask));
        auto* writePage = FastPageOf(memory, target, chunk);
        std::memset(writePage->Write + (target & kMemPageMask), value, chunk);
        memory.NoteWrite(target, chunk);
        done += chunk;
    }
}

} // namespace

size_t Oot3dGuestOverrideCount() noexcept { return kOverrideCount; }

const Oot3dGuestOverrideInfo* Oot3dGuestOverrideTable() noexcept {
    return gOverrides;
}

namespace GuestOverrides {

// ---------------------------------------------------------------------------
// 0x00372070 oot3d_translate_mtx3x4_local (leaf, 1 block)
//   vldmia r1, {s0-s11}; vldmia r2, {s12-s14}
//   s3/s7/s11 += row · v (unfused VMLA, column by column order)
//   vstmia r0, {s0-s11}; bx lr
bool Override_00372070(Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,
                       Oot3dAotArchitecturalState& state,
                       Oot3dWholeAotFlow& flow) noexcept {
    constexpr size_t kIdx = kIdx00372070;
    auto& guest = frame.Guest;
    auto& memory = context.Memory;
    const uint32_t r0 = state.R[0], r1 = state.R[1], r2 = state.R[2];
    if (!Budget(context, 1U) || Hooked(context, 0x00372070U, 0x003720A0U) ||
        !VfpDefault(guest.fpscr) || !Readable(memory, r1, 48U) ||
        !Readable(memory, r2, 12U) || !Writable(memory, r0, 48U)) {
        return Decline(kIdx);
    }
    uint32_t m[12];
    uint32_t v[3];
    for (uint32_t i = 0; i < 12U; ++i) m[i] = Read32(memory, r1 + 4U * i);
    for (uint32_t i = 0; i < 3U; ++i) v[i] = Read32(memory, r2 + 4U * i);
    uint32_t fpscr = guest.fpscr;
    const auto mla = [&](uint32_t& acc, uint32_t a, uint32_t b) {
        Vfp::MultiplyAccumulate(acc, acc, a, b, fpscr);
    };
    mla(m[3], m[0], v[0]);
    mla(m[7], m[4], v[0]);
    mla(m[11], m[8], v[0]);
    mla(m[3], m[1], v[1]);
    mla(m[7], m[5], v[1]);
    mla(m[11], m[9], v[1]);
    mla(m[3], m[2], v[2]);
    mla(m[7], m[6], v[2]);
    mla(m[11], m[10], v[2]);
    for (uint32_t i = 0; i < 12U; ++i) {
        memory.WriteFast<uint32_t>(r0 + 4U * i, m[i]);
    }
    std::copy(m, m + 12, guest.vfp.begin());
    std::copy(v, v + 3, guest.vfp.begin() + 12);
    guest.fpscr = fpscr;
    Consume(context, 1U);
    flow = Oot3dAotReturned(state.R[14]);
    return Accept(kIdx);
}

// ---------------------------------------------------------------------------
// 0x002CDE90 (leaf): if (*(u8*)r0 == 0 && *(f32*)(r0+16) == 0.0f) return s0=0;
//                     else s0 = *(f32*)(r0+4) * 0.1f
bool Override_002CDE90(Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,
                       Oot3dAotArchitecturalState& state,
                       Oot3dWholeAotFlow& flow) noexcept {
    constexpr size_t kIdx = kIdx002CDE90;
    auto& guest = frame.Guest;
    auto& memory = context.Memory;
    const uint32_t r0 = state.R[0];
    if (!Budget(context, 3U) || Hooked(context, 0x002CDE90U, 0x002CDEB8U) ||
        !VfpDefault(guest.fpscr) || !Readable(memory, r0, 1U, 1U)) {
        return Decline(kIdx);
    }
    uint8_t byte = 0U;
    memory.ReadFast<uint8_t>(r0, &byte);
    if (byte == 0U) {
        if (!Readable(memory, r0 + 16U, 4U)) return Decline(kIdx);
    } else if (!Readable(memory, r0 + 4U, 4U)) {
        return Decline(kIdx);
    }
    auto& flags = state.Flags;
    uint32_t fpscr = guest.fpscr;
    state.R[1] = byte;
    flags.SetSubtract(byte, 0U, Mask(0xFU));
    uint32_t blocks = 2U; // 0x002CDE90 and 0x002CDEB8
    bool equal = false;
    if (byte == 0U) {
        const uint32_t s1 = Read32(memory, r0 + 16U);
        guest.vfp[1] = s1;
        guest.vfp[0] = 0x00000000U; // literal at 0x002CDEBC
        Compare(fpscr, flags, s1, 0x00000000U, false);
        equal = flags.Z();
    }
    if (!equal) {
        ++blocks; // 0x002CDEAC
        const uint32_t s0 = Read32(memory, r0 + 4U);
        guest.vfp[1] = 0x3DCCCCCDU; // literal at 0x002CDEC0 (0.1f)
        Vfp::Multiply(guest.vfp[0], s0, 0x3DCCCCCDU, fpscr);
    }
    guest.fpscr = fpscr;
    Consume(context, blocks);
    flow = Oot3dAotReturned(state.R[14]);
    return Accept(kIdx);
}

// ---------------------------------------------------------------------------
// 0x003094CC (leaf): r1 = r0 + r1*4; s0 = max(s0, 0) (VCMPE/vmovlo);
//   if (*(f32*)(r1+72) != s0) { *(f32*)(r1+72) = s0; *(u16*)(r0+0x20) |= 8; }
bool Override_003094CC(Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,
                       Oot3dAotArchitecturalState& state,
                       Oot3dWholeAotFlow& flow) noexcept {
    constexpr size_t kIdx = kIdx003094CC;
    auto& guest = frame.Guest;
    auto& memory = context.Memory;
    const uint32_t r0 = state.R[0];
    const uint32_t slot = r0 + (state.R[1] << 2U);
    if (!Budget(context, 3U) || Hooked(context, 0x003094CCU, 0x00309500U) ||
        !VfpDefault(guest.fpscr) || !Readable(memory, slot + 72U, 4U)) {
        return Decline(kIdx);
    }
    Oot3dAotLazyFlags flags = state.Flags; // local: may still decline
    uint32_t fpscr = guest.fpscr;
    uint32_t s0 = guest.vfp[0];
    Compare(fpscr, flags, s0, 0x00000000U, true); // literal 0.0f at 0x00309504
    if (!flags.C()) s0 = 0x00000000U;
    const uint32_t current = Read32(memory, slot + 72U);
    Compare(fpscr, flags, current, s0, false);
    const bool store = !flags.Z();
    if (store && (!Writable(memory, slot + 72U, 4U) ||
                  !Writable(memory, r0 + 0x20U, 2U, 2U))) {
        return Decline(kIdx);
    }
    // No more declines from here on.
    state.Flags = flags;
    state.R[1] = slot;
    guest.vfp[0] = s0;
    guest.vfp[1] = current;
    uint32_t blocks = 2U; // 0x003094CC and 0x00309500
    if (store) {
        ++blocks; // 0x003094F0
        memory.WriteFast<uint32_t>(slot + 72U, s0);
        uint16_t half = 0U;
        memory.ReadFast<uint16_t>(r0 + 0x20U, &half);
        const uint32_t r1 = static_cast<uint32_t>(half) | 0x00000008U;
        state.R[1] = r1;
        memory.WriteFast<uint16_t>(r0 + 0x20U, static_cast<uint16_t>(r1));
    }
    guest.fpscr = fpscr;
    Consume(context, blocks);
    flow = Oot3dAotReturned(state.R[14]);
    return Accept(kIdx);
}

// ---------------------------------------------------------------------------
// 0x002F9430 PauseRenderBuffer_SetVec2Range (leaf, loop)
//   copies n = r2 float pairs from r1 to buffer(*(r0+0x1C)) + 8*r3 - 4,
//   with the early read of the next pair's first float (including on the
//   last iteration, as in the original).
bool Override_002F9430(Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,
                       Oot3dAotArchitecturalState& state,
                       Oot3dWholeAotFlow& flow) noexcept {
    constexpr size_t kIdx = kIdx002F9430;
    auto& guest = frame.Guest;
    auto& memory = context.Memory;
    auto& flags = state.Flags;
    uint32_t r0 = state.R[0], r1 = state.R[1];
    uint32_t r2 = state.R[2] << 1U;
    uint32_t r3 = state.R[3] << 1U;
    if (Hooked(context, 0x002F9430U, 0x002F9480U)) return Decline(kIdx);
    const bool positive = static_cast<int32_t>(r2) > 0;
    if (!positive) {
        if (!Budget(context, 2U)) return Decline(kIdx);
        flags.SetSubtract(r2, 0U, Mask(0xFU));
        state.R[2] = r2;
        state.R[3] = r3;
        Consume(context, 2U);
        flow = Oot3dAotReturned(state.R[14]);
        return Accept(kIdx);
    }
    const uint32_t count = static_cast<uint32_t>(static_cast<int32_t>(r2) >> 1);
    // 0x002F9430 + 0x002F9440 + count iterations + 0x002F9480
    if (count > 0x00FFFFFFU || !Budget(context, count + 3U) ||
        !Readable(memory, r0 + 0x1CU, 4U)) {
        return Decline(kIdx);
    }
    const uint32_t base = Read32(memory, r0 + 0x1CU);
    const uint32_t destination = base + (0xFFFFFFFCU + (r3 << 2U));
    // Reads in [r1, r1 + 8n + 4), writes in [dest + 4, dest + 8n + 4).
    if (!Readable(memory, r1, 8U * count + 4U) ||
        !Writable(memory, destination + 4U, 8U * count)) {
        return Decline(kIdx);
    }
    uint32_t s0 = Read32(memory, r1); // [r1 - 4 + 4]
    uint32_t s1 = guest.vfp[1];
    r1 -= 4U;
    r0 = destination;
    r3 = destination;
    r2 = count;
    while (true) {
        r1 += 8U;
        r3 = r0;
        s1 = Read32(memory, r1);
        r0 += 8U;
        memory.WriteFast<uint32_t>(r3 + 4U, s0);
        s0 = Read32(memory, r1 + 4U);
        flags.SetSubtract(r2, 1U, Mask(0xFU));
        r2 -= 1U;
        memory.WriteFast<uint32_t>(r0, s1);
        if (flags.Z()) break;
    }
    state.R[0] = r0;
    state.R[1] = r1;
    state.R[2] = r2;
    state.R[3] = r3;
    state.R[12] = 0xFFFFFFFCU;
    guest.vfp[0] = s0;
    guest.vfp[1] = s1;
    Consume(context, count + 3U);
    flow = Oot3dAotReturned(state.R[14]);
    return Accept(kIdx);
}

// ---------------------------------------------------------------------------
// 0x0036E168 Math_SmoothStepToF (inventory name; it is a bounded step
// toward the target, scaled by the global update rate R_UPDATE_RATE).
//   s4 = *pValue; if (s4 == target) -> 0x0036E26C
//   rate = (s16)*(*(ctx**)0x0051B2F4 + 0x110); s3/s1/s2 *= rate/3
//   step = (target - s4) * fraction (|target - s4| < 1e-5 uses the whole remainder),
//   clamped by max/min, written to *pValue; s0 = |target - *pValue|
//
// The arithmetic uses the SAME calls as the generated code (Vfp::, in the
// same order and with the same operands; see namespace Vfp). It used to use
// Oot3dVfpAdd/Multiply (oot3d_native_a32_vfp_fast.h) on AArch64 too, whose
// fast path does host float math: on the console (and in qemu) it runs
// inside Oot3dGuestFpScope, with the guest FPCR and FPSR folded into the
// guest FPSCR on exit, so the flags of the auxiliary computations leaked
// into the guest. The TwoSum of the subtraction s0 - s4 (`sum - a`) overflows
// when |s4| is near FLT_MAX and the sum rounds outward (e.g.
// 0x7EC70697 - 0x7F7FFFFF): extra OFC (and IOC from inf - inf), with FZ=0/RN,
// which the translation does not raise (regression case in the differential test).
// The first comparison, before the last possible decline, stays in software
// (Compare: locals only), matching the native value and flags.
bool Override_0036E168(Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,
                       Oot3dAotArchitecturalState& state,
                       Oot3dWholeAotFlow& flow) noexcept {
    constexpr size_t kIdx = kIdx0036E168;
    auto& guest = frame.Guest;
    auto& memory = context.Memory;
    const uint32_t r0 = state.R[0];
    if (!Budget(context, 8U) || Hooked(context, 0x0036E168U, 0x0036E278U) ||
        !VfpDefault(guest.fpscr) || !Writable(memory, r0, 4U) ||
        !Readable(memory, r0, 4U) || !Readable(memory, 0x0051B2F4U, 4U)) {
        return Decline(kIdx);
    }
    uint32_t fpscr = guest.fpscr;
    Oot3dAotLazyFlags flags = state.Flags; // local: may still decline
    uint32_t s0 = guest.vfp[0], s1 = guest.vfp[1], s2 = guest.vfp[2];
    uint32_t s3 = guest.vfp[3], s5 = guest.vfp[5], s6 = guest.vfp[6];
    uint32_t r1 = state.R[1], r2 = state.R[2];
    const uint32_t s4 = Read32(memory, r0);
    Compare(fpscr, flags, s4, s0, false);
    uint32_t blocks = 2U; // 0x0036E168 and 0x0036E26C
    // VCMP(E) + VMRS APSR_nzcv as in the generated code.
    const auto compare = [&](uint32_t left, uint32_t right) noexcept {
        Oot3dVfpNativeCompare(left, right, true, fpscr);
        flags.AssignPackedNzcv(fpscr);
    };
    if (!flags.Z()) {
        blocks += 1U; // 0x0036E178
        const uint32_t globalContext = Read32(memory, 0x0051B2F4U);
        if (!Readable(memory, globalContext + 0x110U, 2U, 2U)) return Decline(kIdx);
        // No more declines from here on (r0 already validated for writing).
        uint16_t rate16 = 0U;
        memory.ReadFast<uint16_t>(globalContext + 0x110U, &rate16);
        r2 = 0x3727C5ACU; // 1e-5
        r1 = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(rate16)));
        s6 = r1;
        s5 = r1;
        Vfp::FromSigned(s6, s6, fpscr);
        Vfp::FromSigned(s5, s5, fpscr);
        Vfp::Multiply(s3, s6, s3, fpscr);
        s6 = r1;
        Vfp::Multiply(s1, s5, s1, fpscr);
        s5 = 0x3EAAAAABU; // literal at 0x0036E280 (1/3)
        Vfp::FromSigned(s6, s6, fpscr);
        Vfp::Multiply(s3, s3, s5, fpscr);
        Vfp::Multiply(s1, s1, s5, fpscr);
        Vfp::Multiply(s2, s6, s2, fpscr);
        Vfp::Multiply(s2, s2, s5, fpscr);
        Vfp::Subtract(s5, s0, s4, fpscr);
        s6 = s5 & 0x7FFFFFFFU;
        Vfp::Multiply(s1, s5, s1, fpscr);
        r1 = s6;
        flags.SetSubtract(r1, r2, Mask(0x9U));
        if (flags.N() != flags.V()) s1 = s5;
        compare(s1, s3);
        bool toClamp = flags.N() == flags.V(); // ge -> 0x0036E1F4
        if (!toClamp) {
            blocks += 1U; // 0x0036E1E4
            s5 = s3 ^ 0x80000000U;
            compare(s1, s5);
            toClamp = !(flags.C() && !flags.Z()); // hi -> 0x0036E21C
        }
        if (toClamp) {
            blocks += 1U; // 0x0036E1F4
            compare(s1, s2);
            if (!flags.Z() && flags.N() == flags.V()) s1 = s2;
            s2 = s2 ^ 0x80000000U;
            compare(s1, s2);
            if (!flags.C()) s1 = s2;
            Vfp::Add(s1, s4, s1, fpscr);
            memory.WriteFast<uint32_t>(r0, s1);
        } else {
            blocks += 1U; // 0x0036E21C
            compare(s1, s3);
            if (!flags.C()) {
                blocks += 1U; // 0x0036E228
                s1 = s3;
                Vfp::Add(s2, s4, s1, fpscr);
                memory.WriteFast<uint32_t>(r0, s2);
                compare(s2, s0);
                if (!flags.Z() && flags.N() == flags.V()) s2 = s0;
                memory.WriteFast<uint32_t>(r0, s2);
            }
            blocks += 1U; // 0x0036E244
            compare(s1, s5);
            if (!flags.Z() && flags.N() == flags.V()) {
                blocks += 1U; // 0x0036E250
                s1 = Read32(memory, r0);
                Vfp::Add(s1, s1, s5, fpscr);
                memory.WriteFast<uint32_t>(r0, s1);
                compare(s1, s0);
                if (flags.Z() || flags.N() != flags.V()) s1 = s0;
                memory.WriteFast<uint32_t>(r0, s1);
            }
        }
    }
    // 0x0036E26C
    s1 = Read32(memory, r0);
    Vfp::Subtract(s0, s0, s1, fpscr);
    s0 &= 0x7FFFFFFFU;
    state.Flags = flags;
    state.R[1] = r1;
    state.R[2] = r2;
    guest.vfp[0] = s0;
    guest.vfp[1] = s1;
    guest.vfp[2] = s2;
    guest.vfp[3] = s3;
    guest.vfp[4] = s4;
    guest.vfp[5] = s5;
    guest.vfp[6] = s6;
    guest.fpscr = fpscr;
    Consume(context, blocks);
    flow = Oot3dAotReturned(state.R[14]);
    return Accept(kIdx);
}

// ---------------------------------------------------------------------------
// 0x003325F0 Math3D_CylOutsideCylDist (leaf)
//   Copies both cylinders to the global scratch 0x00569830..0x0056985F,
//   dist = sqrt(dx*dx + dz*dz) -> *r3; if they overlap in radius and in
//   height, *r2 = (radius0 + radius1) - dist and r0 = 1, else r0 = 0.
bool Override_003325F0(Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,
                       Oot3dAotArchitecturalState& state,
                       Oot3dWholeAotFlow& flow) noexcept {
    constexpr size_t kIdx = kIdx003325F0;
    constexpr uint32_t kScratch = 0x00569830U; // r4 after the sub
    constexpr uint32_t kSecond = kScratch + 0x24U; // r7 = 0x00569854
    constexpr uint32_t kCopy = kSecond - 12U;     // r0 = 0x00569848
    auto& guest = frame.Guest;
    auto& memory = context.Memory;
    const uint32_t in0 = state.R[0], in1 = state.R[1];
    const uint32_t out5 = state.R[2], out6 = state.R[3];
    const uint32_t sp = state.R[13];
    if (!Budget(context, 5U) || Hooked(context, 0x003325F0U, 0x003326E8U) ||
        !VfpDefault(guest.fpscr) || !Writable(memory, sp - 16U, 16U) ||
        !Readable(memory, sp - 16U, 16U) || !Writable(memory, kScratch, 0x30U) ||
        !Readable(memory, kScratch, 0x30U) || !Readable(memory, in0, 24U) ||
        !Readable(memory, in1, 24U) || !Writable(memory, out6, 4U) ||
        !Writable(memory, out5, 4U)) {
        return Decline(kIdx);
    }
    // push {r4-r7}
    for (uint32_t i = 0; i < 4U; ++i) {
        memory.WriteFast<uint32_t>(sp - 16U + 4U * i, state.R[4 + i]);
    }
    // ldm r0+12 / stm 0x0056983C
    uint32_t r3 = Read32(memory, in0 + 12U);
    uint32_t r7 = Read32(memory, in0 + 16U);
    uint32_t r12 = Read32(memory, in0 + 20U);
    memory.WriteFast<uint32_t>(kScratch + 12U, r3);
    memory.WriteFast<uint32_t>(kScratch + 16U, r7);
    memory.WriteFast<uint32_t>(kScratch + 20U, r12);
    uint32_t s0 = Read32(memory, in0);
    memory.WriteFast<uint32_t>(kScratch, s0);
    s0 = Read32(memory, in0 + 8U);
    memory.WriteFast<uint32_t>(kScratch + 8U, s0);
    s0 = Read32(memory, in0 + 4U);
    memory.WriteFast<uint32_t>(kScratch + 4U, s0);
    // ldm r1+12 / stm 0x00569854
    const uint32_t r2 = Read32(memory, in1 + 12U);
    r3 = Read32(memory, in1 + 16U);
    r12 = Read32(memory, in1 + 20U);
    memory.WriteFast<uint32_t>(kSecond, r2);
    memory.WriteFast<uint32_t>(kSecond + 4U, r3);
    memory.WriteFast<uint32_t>(kSecond + 8U, r12);
    s0 = Read32(memory, in1);
    memory.WriteFast<uint32_t>(kCopy, s0);
    s0 = Read32(memory, in1 + 8U);
    memory.WriteFast<uint32_t>(kCopy + 8U, s0);
    s0 = Read32(memory, in1 + 4U);
    memory.WriteFast<uint32_t>(kCopy + 4U, s0);
    uint32_t fpscr = guest.fpscr;
    Oot3dAotLazyFlags flags = state.Flags;
    uint32_t s1 = Read32(memory, kScratch + 12U);
    s0 = Read32(memory, kCopy + 12U);
    uint32_t s2 = Read32(memory, kCopy + 20U);
    Vfp::Subtract(s1, s1, s0, fpscr);
    s0 = Read32(memory, kScratch + 20U);
    Vfp::Subtract(s0, s0, s2, fpscr);
    Vfp::Multiply(s1, s1, s1, fpscr);
    Vfp::MultiplyAccumulate(s1, s1, s0, s0, fpscr);
    Vfp::SquareRoot(s0, s1, fpscr);
    memory.WriteFast<uint32_t>(out6, s0);
    s1 = Read32(memory, kScratch);
    s2 = Read32(memory, kCopy);
    Vfp::Add(s1, s1, s2, fpscr);
    Compare(fpscr, flags, s1, s0, true);
    uint32_t s3 = guest.vfp[3], s4 = guest.vfp[4], s5 = guest.vfp[5];
    uint32_t result = 0U;
    uint32_t blocks = 3U; // 0x003325F0, 0x003326E4 and (0x003326E0 or 0x003326C0)
    if (flags.C()) {
        blocks += 1U; // 0x00332694
        s2 = Read32(memory, kScratch + 16U);
        s3 = Read32(memory, kScratch + 8U);
        s5 = Read32(memory, kCopy + 8U);
        Vfp::Add(s2, s2, s3, fpscr);
        s3 = Read32(memory, kScratch + 4U);
        Vfp::Add(s4, s2, s3, fpscr);
        s3 = Read32(memory, kCopy + 16U);
        Vfp::Add(s3, s3, s5, fpscr);
        Compare(fpscr, flags, s4, s3, true);
        if (flags.C()) {
            // 0x003326C0
            s4 = Read32(memory, kCopy + 4U);
            Vfp::Add(s3, s3, s4, fpscr);
            Compare(fpscr, flags, s2, s3, true);
            if (flags.Z() || flags.N() != flags.V()) {
                Vfp::Subtract(s0, s1, s0, fpscr);
                result = 1U;
                memory.WriteFast<uint32_t>(out5, s0);
            } else {
                blocks += 1U; // 0x003326E0
            }
        }
    }
    // pop {r4-r7}
    for (uint32_t i = 0; i < 4U; ++i) {
        state.R[4 + i] = Read32(memory, sp - 16U + 4U * i);
    }
    state.R[0] = result;
    state.R[2] = r2;
    state.R[3] = r3;
    state.R[12] = r12;
    state.Flags = flags;
    guest.vfp[0] = s0;
    guest.vfp[1] = s1;
    guest.vfp[2] = s2;
    guest.vfp[3] = s3;
    guest.vfp[4] = s4;
    guest.vfp[5] = s5;
    guest.fpscr = fpscr;
    Consume(context, blocks);
    flow = Oot3dAotReturned(state.R[14]);
    return Accept(kIdx);
}

// ---------------------------------------------------------------------------
// 0x00307C94 PicaCommandWriter_UploadVertexFloatUniforms (leaf, loop)
//   r0 = writer (list at *(r0+8)), r1 = config register, r2 = n vec4,
//   r3 = source. Writes the config header, the data header and the
//   n vectors with components reversed (w, z, y, x), terminates the list with
//   a zero and advances *(r0+8).
bool Override_00307C94(Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,
                       Oot3dAotArchitecturalState& state,
                       Oot3dWholeAotFlow& flow) noexcept {
    constexpr size_t kIdx = kIdx00307C94;
    auto& guest = frame.Guest;
    auto& memory = context.Memory;
    const uint32_t r0 = state.R[0];
    const uint32_t count = state.R[2];
    if (Hooked(context, 0x00307C94U, 0x00307D80U)) return Decline(kIdx);
    if (static_cast<int32_t>(count) < 1) {
        // cmp r2, #1; bxlt lr
        if (!Budget(context, 1U)) return Decline(kIdx);
        state.Flags.SetSubtract(count, 1U, Mask(0xFU));
        Consume(context, 1U);
        flow = Oot3dAotReturned(state.R[14]);
        return Accept(kIdx);
    }
    const uint32_t sp = state.R[13];
    const uint32_t source = state.R[3];
    const uint32_t blocks = count == 1U ? 3U : count + 4U;
    if (count > 0x00FFFFFFU || !Budget(context, blocks) ||
        !Writable(memory, sp - 16U, 16U) || !Readable(memory, sp - 16U, 16U) ||
        !Writable(memory, r0 + 8U, 4U) || !Readable(memory, r0 + 8U, 4U) ||
        (r0 + 8U >= sp - 16U && r0 + 8U < sp)) {
        return Decline(kIdx);
    }
    const uint32_t list = Read32(memory, r0 + 8U);
    const uint32_t listBytes = 28U + 16U * (count - 1U) + 4U;
    if (!Writable(memory, list, listBytes) || !Readable(memory, list, listBytes) ||
        !Readable(memory, source, 16U * count)) {
        return Decline(kIdx);
    }
    // push {r4-r7}
    for (uint32_t i = 0; i < 4U; ++i) {
        memory.WriteFast<uint32_t>(sp - 16U + 4U * i, state.R[4 + i]);
    }
    uint32_t r12 = Read32(memory, r0 + 8U); // same value as `list`
    const uint32_t r2x4 = count << 2U;
    const uint32_t r7 = source + 16U;
    memory.WriteFast<uint32_t>(r12, state.R[1] | 0x80000000U);
    memory.WriteFast<uint32_t>(r12 + 4U, 0x000F02C0U);
    uint32_t s0 = Read32(memory, source + 12U);
    memory.WriteFast<uint32_t>(r12 + 8U, s0);
    memory.WriteFast<uint32_t>(r12 + 12U, ((r2x4 << 20U) - 0x00100000U) | 0x000F02C1U);
    s0 = Read32(memory, source + 8U);
    memory.WriteFast<uint32_t>(r12 + 16U, s0);
    uint32_t r1 = 0xFFFFFFFDU - (r2x4 - 4U);
    uint32_t r2 = Oot3dAotAsr(r1, 31U);
    s0 = Read32(memory, source + 4U);
    memory.WriteFast<uint32_t>(r12 + 20U, s0);
    r1 = r1 + Oot3dAotLsr(r2, 30U);
    s0 = Read32(memory, source);
    const uint32_t r6 = Oot3dAotAsr(r1, 2U);
    uint32_t r3 = 0U - r6;
    Oot3dAotLazyFlags flags = state.Flags;
    flags.SetSubtract(r3, 0U, Mask(0xFU));
    uint32_t r5 = r12 + 28U;
    memory.WriteFast<uint32_t>(r12 + 24U, s0);
    r12 += 20U;
    if (!flags.Z() && flags.N() == flags.V()) {
        r1 = r7 + 12U;
        r2 = r5;
        while (true) {
            s0 = Read32(memory, r1);
            r12 = r2;
            const uint32_t r4 = r1;
            memory.WriteFast<uint32_t>(r12, s0);
            s0 = Read32(memory, r4 - 4U);
            flags.SetSubtract(r3, 1U, Mask(0xFU));
            r3 -= 1U;
            memory.WriteFast<uint32_t>(r12 + 4U, s0);
            s0 = Read32(memory, r4 - 8U);
            r1 += 16U;
            memory.WriteFast<uint32_t>(r12 + 8U, s0);
            s0 = Read32(memory, r4 - 12U);
            r2 += 16U;
            memory.WriteFast<uint32_t>(r12 + 12U, s0);
            if (flags.Z()) break;
        }
        r5 -= r6 << 4U;
    }
    memory.WriteFast<uint32_t>(r5, 0U);
    r1 = r5 + 4U;
    memory.WriteFast<uint32_t>(r0 + 8U, r1);
    // pop {r4-r7}
    for (uint32_t i = 0; i < 4U; ++i) {
        state.R[4 + i] = Read32(memory, sp - 16U + 4U * i);
    }
    state.R[1] = r1;
    state.R[2] = r2;
    state.R[3] = r3;
    state.R[12] = r12;
    state.Flags = flags;
    guest.vfp[0] = s0;
    Consume(context, blocks);
    flow = Oot3dAotReturned(state.R[14]);
    return Accept(kIdx);
}

// ---------------------------------------------------------------------------
// 0x003727F0 Math_SinF
//   Native paths: |x| < 12867.57 (reduction by 2/pi with four-constant
//   Cody-Waite and sine/cosine polynomials) and tiny x (including the calls
//   to 0x002C0B34, classification, and 0x002C0B24, which raises
//   underflow). Declines |x| >= 12867.57, NaN and infinity (call to the
//   reduction 0x002C0B68) and lr inside the function itself. The return is
//   ldr pc, [sp], #4: as in the translation, it exits with Branch(lr).
static void Classify002C0B34(Oot3dAotArchitecturalState& state,
                             Oot3dAotLazyFlags& flags, uint32_t& s0) noexcept {
    uint32_t r0 = state.R[0];
    s0 = r0; // vmov s0, r0
    r0 = s0; // vmov r0, s0
    uint32_t r1 = Oot3dAotLsl(r0, 1U);
    auto shifted = Oot3dAotShiftImmediate(r1, 0U, 8U, flags.C());
    r0 = shifted.Value;
    flags.SetLogical(r0, shifted.Carry, Mask(0x4U));
    if (!flags.Z()) r0 = 0x00000004U;
    shifted = Oot3dAotShiftImmediate(r1, 1U, 24U, flags.C());
    uint32_t r2 = shifted.Value;
    flags.SetLogical(r2, shifted.Carry, Mask(0x6U));
    if (!flags.Z()) r0 |= 0x00000001U;
    r2 = 0xFF000000U;
    shifted = Oot3dAotShiftImmediate(r1, 0U, 0U, flags.C());
    r1 = r2 & ~shifted.Value;
    flags.SetLogical(r1, shifted.Carry, Mask(0x4U));
    if (flags.Z()) r0 |= 0x00000002U;
    flags.SetSubtract(r0, 0x00000001U, Mask(0xFU));
    if (flags.Z()) r0 = 0x00000005U;
    state.R[0] = r0;
    state.R[1] = r1;
    state.R[2] = r2;
}

bool Override_003727F0(Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,
                       Oot3dAotArchitecturalState& state,
                       Oot3dWholeAotFlow& flow) noexcept {
    constexpr size_t kIdx = kIdx003727F0;
    auto& guest = frame.Guest;
    auto& memory = context.Memory;
    const uint32_t x = guest.vfp[0];
    const uint32_t lr = state.R[14];
    const uint32_t sp = state.R[13];
    const uint32_t twice = x << 1U;
    const bool small = twice < 0x7E921FB6U; // |x| < pi/4
    const bool reduceCall = !small && (x & 0x7FFFFFFFU) >= 0x46490E49U;
    if (reduceCall || (lr >= 0x003727F0U && lr <= 0x00372974U) ||
        !Budget(context, 9U) || Hooked(context, 0x003727F0U, 0x00372974U) ||
        Hooked(context, 0x002C0B24U, 0x002C0B64U) || !VfpDefault(guest.fpscr) ||
        !Writable(memory, sp - 16U, 16U) || !Readable(memory, sp - 16U, 16U)) {
        return Decline(kIdx);
    }
    uint32_t fpscr = guest.fpscr;
    Oot3dAotLazyFlags flags = state.Flags;
    uint32_t s0 = x, s1 = guest.vfp[1], s2 = guest.vfp[2], s3 = guest.vfp[3];
    uint32_t r0 = 0x7E921FB6U;
    const uint32_t r1In = x;
    uint32_t r1 = r1In;
    uint32_t r2 = state.R[2];
    uint32_t r14 = lr;
    uint32_t blocks = 1U; // 0x003727F0
    // str lr, [sp, #-4]!; vpush {d8}; cmp r0, r1, lsl #1; sub sp, sp, #4
    memory.WriteFast<uint32_t>(sp - 4U, lr);
    memory.WriteFast<uint32_t>(sp - 12U, guest.vfp[16]);
    memory.WriteFast<uint32_t>(sp - 8U, guest.vfp[17]);
    flags.SetSubtract(r0, r1In << 1U, Mask(0x6U));
    const uint32_t frameSp = sp - 16U;
    if (flags.C() && !flags.Z()) {
        // 0x0037280C: |x| < pi/4; r0 = -1 if |x| is tiny
        blocks += 1U;
        r0 = 0x73000000U;
        flags.SetSubtract(r0, r1In << 1U, Mask(0x6U));
        if (!flags.C() || flags.Z()) r0 = 0x00000000U;
        if (flags.C() && !flags.Z()) r0 = 0xFFFFFFFFU;
        memory.WriteFast<uint32_t>(frameSp, r0);
    } else {
        // 0x00372824 and 0x00372834 (the large reduction case was declined)
        blocks += 2U;
        r2 = 0x46490E49U;
        r0 = r1In & ~0x80000000U;
        flags.SetSubtract(r0, r2, Mask(0xFU));
        flags.SetLogical(r1In & 0x80000000U, true, Mask(0x4U));
        s1 = 0x3F22F983U; // 2/pi
        s2 = 0x4B000000U; // 2^23
        s3 = 0x39FDA000U;
        Vfp::Multiply(s1, s0, s1, fpscr);
        if (!flags.Z()) Vfp::Subtract(s1, s1, s2, fpscr);
        Vfp::Add(s1, s1, s2, fpscr);
        if (flags.Z()) Vfp::Subtract(s1, s1, s2, fpscr);
        Vfp::ToSigned(s2, s1, fpscr);
        r0 = s2;
        s2 = 0x3FC90000U;
        r0 &= 0x00000003U;
        Vfp::MultiplySubtract(s0, s0, s1, s2, fpscr);
        s2 = 0x33A22000U;
        memory.WriteFast<uint32_t>(frameSp, r0);
        Vfp::MultiplySubtract(s0, s0, s1, s3, fpscr);
        s3 = 0x2C34611AU;
        Vfp::MultiplySubtract(s0, s0, s1, s2, fpscr);
        Vfp::MultiplySubtract(s0, s0, s1, s3, fpscr);
    }
    // 0x0037288C
    blocks += 1U;
    r0 = Read32(memory, frameSp);
    uint32_t s16 = s0;
    uint32_t s17 = guest.vfp[17];
    flags.SetSubtract(r0, 0U, Mask(0xBU));
    uint32_t returnSp = frameSp + 4U; // where the final vpop {d8} starts
    bool negate = false;
    if (flags.N() != flags.V()) {
        // 0x0037289C: quadrant -1, only written by 0x0037280C with |x| < 2^-12
        // (2|x| < 0x73000000). So 2|x| < 0xFF000000, C is clear and the
        // 0x003728CC branch (NaN/inf) is unreachable from here.
        blocks += 1U;
        r0 = Oot3dAotLsl(s16, 1U);
        flags.SetSubtract(r0, 0xFF000000U, Mask(0xFU));
        {
            // 0x003728AC: bl 0x002C0B34; 0x003728B4: cmp r0, #4; bleq 0x002C0B24
            blocks += 3U;
            r0 = s16;
            OOT3D_AOT_STAT(context.Stats.DirectCalls);
            r14 = 0x003728B4U;
            state.R[0] = r0;
            state.R[1] = r1;
            state.R[2] = r2;
            Classify002C0B34(state, flags, s0);
            r0 = state.R[0];
            r1 = state.R[1];
            r2 = state.R[2];
            flags.SetSubtract(r0, 0x00000004U, Mask(0xFU));
            if (flags.Z()) {
                blocks += 1U; // 0x002C0B24
                OOT3D_AOT_STAT(context.Stats.DirectCalls);
                r14 = 0x003728BCU;
                s0 = 0x10000000U; // literal at 0x002C0B30
                Vfp::Multiply(s0, s0, s0, fpscr);
            }
            blocks += 1U; // 0x003728BC
            s0 = s16;
        }
    } else {
        // 0x003728F8: tst r0, #1
        blocks += 1U;
        flags.SetLogical(r0 & 0x00000001U, flags.C(), Mask(0x6U));
        if (flags.Z()) {
            // 0x00372900: even quadrants (sine polynomial)
            blocks += 1U;
            Vfp::Multiply(s0, s16, s16, fpscr);
            flags.SetLogical(r0 & 0x00000002U, flags.C(), Mask(0xEU));
            s1 = 0x394C6D33U;
            s2 = 0x3C0882DAU;
            Vfp::MultiplySubtract(s2, s2, s0, s1, fpscr);
            s1 = 0xBE2AAAA0U;
            Vfp::MultiplyAccumulate(s1, s1, s0, s2, fpscr);
            Vfp::Multiply(s1, s1, s0, fpscr);
            s0 = s16;
            Vfp::MultiplyAccumulate(s0, s0, s16, s1, fpscr);
            if (!flags.Z()) {
                blocks += 2U; // 0x00372934 (b) and 0x00372968
                negate = true;
            }
        } else {
            // 0x00372938: odd quadrants (cosine polynomial)
            blocks += 1U;
            Vfp::Multiply(s1, s16, s16, fpscr);
            s0 = 0xBAB23AB9U;
            flags.SetLogical(r0 & 0x00000002U, flags.C(), Mask(0xEU));
            s3 = 0x3D2A9FCAU;
            s2 = 0xBEFFFFDDU;
            const bool early = flags.Z();
            if (early) {
                // vpopeq {d8} before the math: reads only, same order.
                s16 = Read32(memory, frameSp + 4U);
                s17 = Read32(memory, frameSp + 8U);
            }
            Vfp::MultiplyAccumulate(s3, s3, s1, s0, fpscr);
            s0 = 0x3F800000U;
            Vfp::MultiplyAccumulate(s2, s2, s1, s3, fpscr);
            Vfp::MultiplyAccumulate(s0, s0, s1, s2, fpscr);
            if (early) {
                const uint32_t target = Read32(memory, frameSp + 12U);
                state.R[0] = r0;
                state.R[1] = r1;
                state.R[2] = r2;
                state.R[13] = frameSp + 16U;
                state.Flags = flags;
                guest.vfp[0] = s0;
                guest.vfp[1] = s1;
                guest.vfp[2] = s2;
                guest.vfp[3] = s3;
                guest.vfp[16] = s16;
                guest.vfp[17] = s17;
                guest.fpscr = fpscr;
                Consume(context, blocks);
                flow = Oot3dAotBranch(target);
                return Accept(kIdx);
            }
            blocks += 1U; // 0x00372968
            negate = true;
        }
    }
    // vpop {d8} (+ vneg.f32 s0, s0 at 0x00372968); ldr pc, [sp], #4
    s16 = Read32(memory, returnSp);
    s17 = Read32(memory, returnSp + 4U);
    if (negate) s0 ^= 0x80000000U;
    const uint32_t target = Read32(memory, returnSp + 8U);
    state.R[0] = r0;
    state.R[1] = r1;
    state.R[2] = r2;
    state.R[13] = returnSp + 12U;
    state.R[14] = r14;
    state.Flags = flags;
    guest.vfp[0] = s0;
    guest.vfp[1] = s1;
    guest.vfp[2] = s2;
    guest.vfp[3] = s3;
    guest.vfp[16] = s16;
    guest.vfp[17] = s17;
    guest.fpscr = fpscr;
    Consume(context, blocks);
    flow = Oot3dAotBranch(target);
    return Accept(kIdx);
}

// ---------------------------------------------------------------------------
// DSP audio effects (integer arithmetic, 160 samples per channel).
// Same contract as the others, with one difference: the block budget is
// honored block by block (LocalGuest::Enter). If it runs out mid-loop, the
// function exits with BlockLimit at the same PC and with the same state the
// translation would have at that point, and the dispatcher resumes through
// the translation. Reads and writes still all go through the same
// ReadFast/WriteFast calls in the same order; the initial validation
// guarantees none fails and that buffer writes reach neither the stack nor
// the object's fields (where the validated pointers and coefficients come from).

// 0x004A022C: delay of up to 4 channels. r0 = effect state, r1 = 4
// input/output pointers. Per sample: d = delay[i]; s = round0(d * gain >> 7);
// y = (k1 * (x - s) + k2 * fb) >> 7; fb = delay[i] = y; x = d.
bool Override_004A022C(Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,
                       Oot3dAotArchitecturalState& state,
                       Oot3dWholeAotFlow& flow) noexcept {
    constexpr size_t kIdx = kIdx004A022C;
    static_cast<void>(frame);
    auto& memory = context.Memory;
    const uint32_t obj = state.R[0];
    const uint32_t list = state.R[1];
    const uint32_t sp0 = state.R[13];
    if (Hooked(context, 0x004A022CU, 0x004A0334U) ||
        !Readable(memory, obj + 0x56U, 1U, 1U)) {
        return Decline(kIdx);
    }
    uint8_t enabled = 0U;
    memory.ReadFast<uint8_t>(obj + 0x56U, &enabled);
    if (enabled != 0U) {
        // Validation of everything the full path accesses.
        const uint32_t stackLow = sp0 - 52U;
        if (!Writable(memory, stackLow, 52U) || !Readable(memory, stackLow, 52U) ||
            !Readable(memory, list, 16U) || !Readable(memory, obj + 0x1CU, 0x3CU) ||
            !Writable(memory, obj + 0x2CU, 16U) || !Writable(memory, obj + 0x40U, 4U) ||
            !Readable(memory, obj + 0x55U, 1U, 1U) ||
            Overlaps(list, 16U, stackLow, 52U) || Overlaps(obj + 0x1CU, 0x3CU, stackLow, 52U)) {
            return Decline(kIdx);
        }
        uint8_t channels = 0U;
        memory.ReadFast<uint8_t>(obj + 0x55U, &channels);
        if (channels > 4U) return Decline(kIdx);
        const uint32_t frameIndex = Read32(memory, obj + 0x40U);
        const uint32_t base = (frameIndex + (frameIndex << 2U)) << 5U;
        for (uint32_t ch = 0; ch < channels; ++ch) {
            const uint32_t io = Read32(memory, list + 4U * ch);
            const uint32_t delay = Read32(memory, obj + 0x1CU + 4U * ch) + (base << 2U);
            for (const uint32_t buffer : {io, delay}) {
                if (!Writable(memory, buffer, 640U) || !Readable(memory, buffer, 640U) ||
                    Overlaps(buffer, 640U, stackLow, 52U) ||
                    Overlaps(buffer, 640U, obj + 0x1CU, 0x3CU)) {
                    return Decline(kIdx);
                }
            }
        }
    }
    LocalGuest g(context, state);
    auto& R = g.R;
    auto& flags = g.Flags;
    // 0x004A022C
    if (!g.Enter()) { flow = g.BlockLimit(0x004A022CU); return Accept(kIdx); }
    R[2] = enabled;
    flags.SetSubtract(R[2], 0U, Mask(0xFU));
    if (flags.Z()) {
        g.Flush();
        flow = Oot3dAotReturned(R[14]);
        return Accept(kIdx);
    }
    // 0x004A0238: push {r4-r11, lr}; sub sp, #16; copy of the 4 pointers
    if (!g.Enter()) { flow = g.BlockLimit(0x004A0238U); return Accept(kIdx); }
    {
        const uint32_t pushed[9] = {R[4], R[5], R[6], R[7], R[8], R[9], R[10], R[11], R[14]};
        for (uint32_t i = 0; i < 9U; ++i) memory.WriteFast<uint32_t>(R[13] - 36U + 4U * i, pushed[i]);
        R[13] -= 36U + 16U;
    }
    R[7] = 0U;
    R[2] = Read32(memory, R[1]);
    memory.WriteFast<uint32_t>(R[13], R[2]);
    R[2] = Read32(memory, R[1] + 4U);
    memory.WriteFast<uint32_t>(R[13] + 4U, R[2]);
    R[2] = Read32(memory, R[1] + 8U);
    memory.WriteFast<uint32_t>(R[13] + 8U, R[2]);
    R[1] = Read32(memory, R[1] + 12U);
    memory.WriteFast<uint32_t>(R[13] + 12U, R[1]);
    R[1] = Read32(memory, R[0] + 0x40U);
    R[1] = R[1] + (R[1] << 2U);
    R[8] = R[1] << 5U;
    {
        uint8_t value = 0U;
        memory.ReadFast<uint8_t>(R[0] + 0x55U, &value);
        R[1] = value;
    }
    flags.SetSubtract(R[1], 0U, Mask(0x6U));
    if (flags.C() && !flags.Z()) {
        // 0x004A027C
        if (!g.Enter()) { flow = g.BlockLimit(0x004A027CU); return Accept(kIdx); }
        R[9] = R[13];
        while (true) {
            // 0x004A0280: channel r7 setup
            if (!g.Enter()) { flow = g.BlockLimit(0x004A0280U); return Accept(kIdx); }
            R[1] = R[0] + (R[7] << 2U);
            R[4] = Read32(memory, R[9] + (R[7] << 2U));
            R[5] = Read32(memory, R[1] + 0x1CU);
            R[6] = R[1] + 44U;
            R[2] = R[8];
            R[1] = 0U;
            // Fast path of the sample loop, in runs between the page
            // crossings of the two buffers: gain, coefficients and channel
            // state (r6 = r0 + 0x2C + 4 * channel) in registers (the buffers
            // do not reach the object: validated on entry); the 3 writes per
            // sample (state, delay, output) are accounted at the end of the
            // run. Each sample is 3 blocks (0x004A0298, 0x004A02BC or
            // 0x004A02A4, 0x004A02C8) and only enters with quota >= 3,
            // as in the per-sample loop.
            Cursor stateCell;
            bool fast = ResolveCursor(memory, R[6], 1U, stateCell);
            const uint32_t gain = fast ? Read32(memory, R[0] + 0x44U) : 0U;
            const uint32_t mix = fast ? Read32(memory, R[0] + 0x48U) : 0U;
            const uint32_t feedback = fast ? Read32(memory, R[0] + 0x4CU) : 0U;
            while (true) {
                if (fast && context.BlocksRemaining >= 3U) {
                    const uint32_t left = 160U - R[1];
                    Cursor delay, io;
                    if (!ResolveCursor(memory, R[5] + (R[2] << 2U), left, delay) ||
                        !ResolveCursor(memory, R[4] + (R[1] << 2U), left, io)) {
                        fast = false;
                        continue;
                    }
                    const uint32_t samples = std::min(
                        {delay.Words, io.Words, context.BlocksRemaining / 3U});
                    uint32_t state = CursorLoad(stateCell, 0U);
                    uint32_t r3 = R[3], r10 = R[10], r11 = R[11], r12 = R[12], r14 = R[14];
                    for (uint32_t i = 0; i < samples; ++i) {
                        r3 = CursorLoad(delay, i);
                        r10 = gain;
                        r12 = static_cast<int32_t>(r3) >= 0
                                  ? Oot3dAotAsr(r10 * r3, 7U)
                                  : 0U - Oot3dAotAsr(r10 * (0U - r3), 7U);
                        r10 = CursorLoad(io, i);
                        r11 = state;
                        r14 = mix;
                        r12 = r10 - r12;
                        r10 = feedback * r11;
                        r12 = Oot3dAotAsr(r14 * r12 + r10, 7U);
                        state = r12;
                        CursorStore(delay, i, r12);
                        CursorStore(io, i, r3);
                    }
                    CursorStore(stateCell, 0U, state);
                    CommitSamples(memory, {&stateCell, &delay, &io});
                    Consume(context, 3U * samples);
                    R[1] += samples;
                    R[2] += samples;
                    R[3] = r3;
                    R[10] = r10;
                    R[11] = r11;
                    R[12] = r12;
                    R[14] = r14;
                    flags.SetSubtract(R[3], 0U, Mask(0x9U));
                    flags.SetSubtract(R[1], 0x000000A0U, Mask(0x2U));
                    if (flags.C()) break;
                    continue;
                }
                // 0x004A0298
                if (!g.Enter()) { flow = g.BlockLimit(0x004A0298U); return Accept(kIdx); }
                R[3] = Read32(memory, R[5] + (R[2] << 2U));
                flags.SetSubtract(R[3], 0U, Mask(0x9U));
                if (flags.N() == flags.V()) {
                    // 0x004A02BC
                    if (!g.Enter()) { flow = g.BlockLimit(0x004A02BCU); return Accept(kIdx); }
                    R[10] = Read32(memory, R[0] + 0x44U);
                    R[12] = R[10] * R[3];
                    R[12] = Oot3dAotAsr(R[12], 7U);
                } else {
                    // 0x004A02A4
                    if (!g.Enter()) { flow = g.BlockLimit(0x004A02A4U); return Accept(kIdx); }
                    R[10] = Read32(memory, R[0] + 0x44U);
                    R[12] = 0U - R[3];
                    R[12] = R[10] * R[12];
                    R[12] = Oot3dAotAsr(R[12], 7U);
                    R[12] = 0U - R[12];
                }
                // 0x004A02C8
                if (!g.Enter()) { flow = g.BlockLimit(0x004A02C8U); return Accept(kIdx); }
                R[10] = Read32(memory, R[4] + (R[1] << 2U));
                R[11] = Read32(memory, R[6]);
                R[14] = Read32(memory, R[0] + 0x48U);
                R[12] = R[10] - R[12];
                R[10] = Read32(memory, R[0] + 0x4CU);
                R[10] = R[10] * R[11];
                R[12] = R[14] * R[12] + R[10];
                R[12] = Oot3dAotAsr(R[12], 7U);
                memory.WriteFast<uint32_t>(R[6], R[12]);
                memory.WriteFast<uint32_t>(R[5] + (R[2] << 2U), R[12]);
                memory.WriteFast<uint32_t>(R[4] + (R[1] << 2U), R[3]);
                R[1] += 1U;
                flags.SetSubtract(R[1], 0x000000A0U, Mask(0x2U));
                R[2] += 1U;
                if (flags.C()) break;
            }
            // 0x004A0304
            if (!g.Enter()) { flow = g.BlockLimit(0x004A0304U); return Accept(kIdx); }
            {
                uint8_t value = 0U;
                memory.ReadFast<uint8_t>(R[0] + 0x55U, &value);
                R[1] = value;
            }
            R[7] += 1U;
            flags.SetSubtract(R[1], R[7], Mask(0x6U));
            if (!(flags.C() && !flags.Z())) break;
        }
    }
    // 0x004A0314: advances the circular frame index and returns
    if (!g.Enter()) { flow = g.BlockLimit(0x004A0314U); return Accept(kIdx); }
    R[1] = Read32(memory, R[0] + 0x40U);
    R[1] += 1U;
    memory.WriteFast<uint32_t>(R[0] + 0x40U, R[1]);
    R[2] = Read32(memory, R[0] + 0x3CU);
    flags.SetSubtract(R[1], R[2], Mask(0xFU));
    if (flags.C()) {
        R[1] = 0U;
        memory.WriteFast<uint32_t>(R[0] + 0x40U, R[1]);
    }
    R[13] += 16U;
    uint32_t popped[9];
    for (uint32_t i = 0; i < 9U; ++i) popped[i] = Read32(memory, R[13] + 4U * i);
    for (uint32_t i = 0; i < 8U; ++i) R[4 + i] = popped[i];
    R[13] += 36U;
    g.Flush();
    flow = Oot3dAotReturned(popped[8]);
    return Accept(kIdx);
}

// 0x004A0338: stereo reverb (2 channels x 160 samples), five circular delay
// lines (indices at 0x9C/0xA4/0xB0/0xB4/0xC4, limits at
// 0x98/0xA0/0xA8/0xAC/0xC0), filters with symmetric rounding (the sign is
// taken before the >> 7) and feedback; the output is written to each
// channel's own input buffer.
bool Override_004A0338(Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,
                       Oot3dAotArchitecturalState& state,
                       Oot3dWholeAotFlow& flow) noexcept {
    constexpr size_t kIdx = kIdx004A0338;
    static_cast<void>(frame);
    auto& memory = context.Memory;
    const uint32_t obj = state.R[0];
    const uint32_t list = state.R[1];
    const uint32_t sp0 = state.R[13];
    if (Hooked(context, 0x004A0338U, 0x004A0570U) ||
        !Readable(memory, obj + 0x100U, 1U, 1U)) {
        return Decline(kIdx);
    }
    uint8_t enabled = 0U;
    memory.ReadFast<uint8_t>(obj + 0x100U, &enabled);
    constexpr uint32_t kObjLow = 0x38U, kObjSize = 0x104U - 0x38U;
    if (enabled != 0U) {
        const uint32_t stackLow = sp0 - 72U;
        if (!Writable(memory, stackLow, 72U) || !Readable(memory, stackLow, 72U) ||
            !Readable(memory, list, 16U) || !Readable(memory, obj + kObjLow, kObjSize - 4U) ||
            !Writable(memory, obj + 0x98U, 0x30U) || !Writable(memory, obj + 0xCCU, 8U) ||
            !SinglePageReadable(memory, obj + 0xB0U, 8U) ||
            Overlaps(list, 16U, stackLow, 72U) ||
            Overlaps(obj + kObjLow, kObjSize, stackLow, 72U)) {
            return Decline(kIdx);
        }
        const uint32_t i3 = Read32(memory, obj + 0x9CU), i12 = Read32(memory, obj + 0xA4U);
        const uint32_t i4 = Read32(memory, obj + 0xB0U), i5 = Read32(memory, obj + 0xB4U);
        const uint32_t i6 = Read32(memory, obj + 0xC4U);
        for (uint32_t ch = 0; ch < 2U; ++ch) {
            const uint32_t buffers[6] = {
                Read32(memory, obj + 0x38U + 4U * ch) + (i3 << 2U),
                Read32(memory, obj + 0x48U + 4U * ch) + (i12 << 2U),
                Read32(memory, obj + 0x58U + 8U * ch) + (i4 << 2U),
                Read32(memory, obj + 0x5CU + 8U * ch) + (i5 << 2U),
                Read32(memory, obj + 0x78U + 4U * ch) + (i6 << 2U),
                Read32(memory, list + 4U * ch),
            };
            for (const uint32_t buffer : buffers) {
                if (!Writable(memory, buffer, 640U) || !Readable(memory, buffer, 640U) ||
                    Overlaps(buffer, 640U, stackLow, 72U) ||
                    Overlaps(buffer, 640U, obj + kObjLow, kObjSize)) {
                    return Decline(kIdx);
                }
            }
        }
    }
    LocalGuest g(context, state);
    auto& R = g.R;
    auto& flags = g.Flags;
    // 0x004A0338
    if (!g.Enter()) { flow = g.BlockLimit(0x004A0338U); return Accept(kIdx); }
    R[2] = enabled;
    flags.SetSubtract(R[2], 0U, Mask(0xFU));
    if (flags.Z()) {
        g.Flush();
        flow = Oot3dAotReturned(R[14]);
        return Accept(kIdx);
    }
    // 0x004A0344: push {r4-r11, lr}; sub sp, #36; copy of the 4 pointers
    if (!g.Enter()) { flow = g.BlockLimit(0x004A0344U); return Accept(kIdx); }
    {
        const uint32_t pushed[9] = {R[4], R[5], R[6], R[7], R[8], R[9], R[10], R[11], R[14]};
        for (uint32_t i = 0; i < 9U; ++i) memory.WriteFast<uint32_t>(R[13] - 36U + 4U * i, pushed[i]);
        R[13] -= 36U + 36U;
    }
    R[10] = 0U;
    R[2] = Read32(memory, R[1]);
    memory.WriteFast<uint32_t>(R[13] + 0x10U, R[2]);
    R[2] = Read32(memory, R[1] + 4U);
    memory.WriteFast<uint32_t>(R[13] + 0x14U, R[2]);
    R[2] = Read32(memory, R[1] + 8U);
    memory.WriteFast<uint32_t>(R[13] + 0x18U, R[2]);
    R[1] = Read32(memory, R[1] + 12U);
    memory.WriteFast<uint32_t>(R[13] + 0x1CU, R[1]);
    while (true) {
        // 0x004A0370: setup of channel r10
        if (!g.Enter()) { flow = g.BlockLimit(0x004A0370U); return Accept(kIdx); }
        R[3] = Read32(memory, R[0] + 0x9CU);
        {
            uint64_t pair = 0U;
            memory.ReadFast<uint64_t>(R[0] + 0xB0U, &pair);
            R[4] = static_cast<uint32_t>(pair);
            R[5] = static_cast<uint32_t>(pair >> 32U);
        }
        R[9] = R[0] + (R[10] << 2U);
        R[6] = Read32(memory, R[0] + 0xC4U);
        R[2] = Read32(memory, R[9] + 0x38U);
        R[12] = Read32(memory, R[0] + 0xA4U);
        R[7] = Read32(memory, R[9] + 0x48U);
        memory.WriteFast<uint32_t>(R[13] + 0xCU, R[2]);
        R[2] = R[0] + (R[10] << 3U);
        R[1] = 0U;
        memory.WriteFast<uint32_t>(R[13] + 0x20U, R[2]);
        memory.WriteFast<uint32_t>(R[13] + 0x8U, R[7]);
        // Fast path of the sample loop (0x004A03A0..0x004A04B4), in
        // chunks between the page crossings of the six buffers: pointers
        // and coefficients in registers, and also the stack words
        // [sp], [sp + 4] and the filter state [r9 + 0xCC], which the loop always
        // writes before reading (the buffers do not reach the stack or the object:
        // validated on entry). The 9 writes per sample are accounted
        // at the end of the chunk; each sample is 7 to 9 blocks and only enters with
        // quota >= 9, as in the per-sample loop.
        Cursor stack0, stack4, filterCell;
        bool fast = ResolveCursor(memory, R[13], 1U, stack0) &&
                    ResolveCursor(memory, R[13] + 4U, 1U, stack4) &&
                    ResolveCursor(memory, R[9] + 0xCCU, 1U, filterCell);
        uint32_t pointerA = 0U, pointerB = 0U, pointerC = 0U, pointerD = 0U, pointerE = 0U;
        uint32_t pointerIo = 0U, channelBase = 0U;
        uint32_t kDC = 0U, kB8 = 0U, kBC = 0U, kC8 = 0U, kE8 = 0U, kE0 = 0U;
        if (fast) {
            pointerA = Read32(memory, R[13] + 0xCU);
            pointerB = Read32(memory, R[13] + 0x8U);
            pointerIo = Read32(memory, R[13] + 0x10U + (R[10] << 2U));
            channelBase = Read32(memory, R[13] + 0x20U);
            pointerC = Read32(memory, channelBase + 0x58U);
            pointerD = Read32(memory, channelBase + 0x5CU);
            pointerE = Read32(memory, R[9] + 0x78U);
            kDC = Read32(memory, R[0] + 0xDCU);
            kB8 = Read32(memory, R[0] + 0xB8U);
            kBC = Read32(memory, R[0] + 0xBCU);
            kC8 = Read32(memory, R[0] + 0xC8U);
            kE8 = Read32(memory, R[0] + 0xE8U);
            kE0 = Read32(memory, R[0] + 0xE0U);
        }
        while (true) {
            if (fast && context.BlocksRemaining >= 9U) {
                const uint32_t left = 160U - R[1];
                Cursor a, b, c, d, e, io;
                if (!ResolveCursor(memory, pointerA + (R[3] << 2U), left, a) ||
                    !ResolveCursor(memory, pointerB + (R[12] << 2U), left, b) ||
                    !ResolveCursor(memory, pointerC + (R[4] << 2U), left, c) ||
                    !ResolveCursor(memory, pointerD + (R[5] << 2U), left, d) ||
                    !ResolveCursor(memory, pointerE + (R[6] << 2U), left, e) ||
                    !ResolveCursor(memory, pointerIo + (R[1] << 2U), left, io)) {
                    fast = false;
                    continue;
                }
                const uint32_t limit =
                    std::min({a.Words, b.Words, c.Words, d.Words, e.Words, io.Words});
                uint32_t remaining = context.BlocksRemaining;
                uint32_t filter = CursorLoad(filterCell, 0U);
                uint32_t st0 = 0U, st4 = 0U;
                uint32_t r1 = R[1], r2 = R[2], r3 = R[3], r4 = R[4], r5 = R[5], r6 = R[6];
                uint32_t r7 = R[7], r8 = R[8], r11 = R[11], r12 = R[12], r14 = R[14];
                uint32_t n = 0U;
                for (; n < limit && remaining >= 9U; ++n) {
                    uint32_t blocks = 7U;
                    r11 = kDC;
                    r8 = CursorLoad(a, n);
                    r2 = CursorLoad(io, n);
                    CursorStore(a, n, r2);
                    r14 = pointerB;
                    r11 = r8 * r11;
                    r7 = CursorLoad(b, n);
                    CursorStore(b, n, r2);
                    r8 = pointerC;
                    r2 = CursorLoad(c, n);
                    st4 = r11;
                    if (static_cast<int32_t>(r2) >= 0) {
                        r11 = Oot3dAotAsr(kB8 * r2, 7U);
                    } else {
                        r14 = kB8;
                        r11 = 0U - Oot3dAotAsr(r14 * (0U - r2), 7U);
                    }
                    r11 = r11 + r7;
                    CursorStore(c, n, r11);
                    st0 = r2;
                    r8 = pointerD;
                    r2 = CursorLoad(d, n);
                    if (static_cast<int32_t>(r2) >= 0) {
                        r11 = Oot3dAotAsr(kBC * r2, 7U);
                    } else {
                        r14 = kBC;
                        r11 = 0U - Oot3dAotAsr(r14 * (0U - r2), 7U);
                    }
                    r7 = r7 + r11;
                    CursorStore(d, n, r7);
                    r7 = st0;
                    r11 = pointerE;
                    r8 = r7 - r2;
                    r2 = kC8;
                    r7 = CursorLoad(e, n);
                    r14 = r7;
                    if (static_cast<int32_t>(r7) >= 0) {
                        r14 = Oot3dAotAsr(r14 * r2, 7U);
                    } else {
                        ++blocks; // 0x004A0480
                        r14 = 0U - Oot3dAotAsr((0U - r14) * r2, 7U);
                    }
                    r8 = r8 + r14;
                    CursorStore(e, n, r8);
                    if (static_cast<int32_t>(r8) >= 0) {
                        r2 = Oot3dAotAsr(r8 * r2, 7U);
                    } else {
                        ++blocks; // 0x004A04A4
                        r8 = 0U - r8;
                        r2 = 0U - Oot3dAotAsr(r8 * r2, 7U);
                    }
                    r8 = filter;
                    r2 = r7 - r2;
                    r7 = kE8;
                    r8 = r8 + r2;
                    r7 = r7 * r8;
                    r2 = r2 - Oot3dAotAsr(r7, 7U);
                    filter = r2;
                    r8 = kE0;
                    r7 = st4;
                    r2 = r2 * r8 + r7;
                    r7 = pointerIo;
                    r2 = Oot3dAotAsr(r2, 7U);
                    CursorStore(io, n, r2);
                    remaining -= blocks;
                }
                CursorStore(stack4, 0U, st4);
                CursorStore(stack0, 0U, st0);
                CursorStore(filterCell, 0U, filter);
                CommitSamples(memory, {&a, &b, &stack4, &c, &stack0, &d, &e, &filterCell, &io});
                context.BlocksRemaining = remaining;
                R[1] = r1 + n;
                R[2] = r2;
                R[3] = r3 + n;
                R[4] = r4 + n;
                R[5] = r5 + n;
                R[6] = r6 + n;
                R[7] = r7;
                R[8] = r8;
                R[11] = r11;
                R[12] = r12 + n;
                R[14] = r14;
                flags.SetSubtract(R[1], 0x000000A0U, Mask(0x9U));
                if (flags.N() == flags.V()) break;
                continue;
            }
            // 0x004A03A0
            if (!g.Enter()) { flow = g.BlockLimit(0x004A03A0U); return Accept(kIdx); }
            R[2] = R[13] + 0x10U;
            R[7] = Read32(memory, R[13] + 0xCU);
            R[2] = Read32(memory, R[2] + (R[10] << 2U));
            R[11] = Read32(memory, R[0] + 0xDCU);
            R[8] = Read32(memory, R[7] + (R[3] << 2U));
            R[2] = Read32(memory, R[2] + (R[1] << 2U));
            memory.WriteFast<uint32_t>(R[7] + (R[3] << 2U), R[2]);
            R[7] = Read32(memory, R[13] + 0x8U);
            R[14] = Read32(memory, R[13] + 0x8U);
            R[11] = R[8] * R[11];
            R[7] = Read32(memory, R[7] + (R[12] << 2U));
            memory.WriteFast<uint32_t>(R[14] + (R[12] << 2U), R[2]);
            R[2] = Read32(memory, R[13] + 0x20U);
            R[8] = Read32(memory, R[2] + 0x58U);
            R[2] = Read32(memory, R[8] + (R[4] << 2U));
            memory.WriteFast<uint32_t>(R[13] + 0x4U, R[11]);
            flags.SetSubtract(R[2], 0U, Mask(0x9U));
            if (flags.N() == flags.V()) {
                // 0x004A0400
                if (!g.Enter()) { flow = g.BlockLimit(0x004A0400U); return Accept(kIdx); }
                R[11] = Read32(memory, R[0] + 0xB8U);
                R[11] = R[11] * R[2];
                R[11] = Oot3dAotAsr(R[11], 7U);
            } else {
                // 0x004A03E8
                if (!g.Enter()) { flow = g.BlockLimit(0x004A03E8U); return Accept(kIdx); }
                R[14] = Read32(memory, R[0] + 0xB8U);
                R[11] = 0U - R[2];
                R[11] = R[14] * R[11];
                R[11] = Oot3dAotAsr(R[11], 7U);
                R[11] = 0U - R[11];
            }
            // 0x004A040C
            if (!g.Enter()) { flow = g.BlockLimit(0x004A040CU); return Accept(kIdx); }
            R[11] = R[11] + R[7];
            memory.WriteFast<uint32_t>(R[8] + (R[4] << 2U), R[11]);
            memory.WriteFast<uint32_t>(R[13], R[2]);
            R[2] = Read32(memory, R[13] + 0x20U);
            R[8] = Read32(memory, R[2] + 0x5CU);
            R[2] = Read32(memory, R[8] + (R[5] << 2U));
            flags.SetSubtract(R[2], 0U, Mask(0x9U));
            if (flags.N() == flags.V()) {
                // 0x004A0444
                if (!g.Enter()) { flow = g.BlockLimit(0x004A0444U); return Accept(kIdx); }
                R[11] = Read32(memory, R[0] + 0xBCU);
                R[11] = R[11] * R[2];
                R[11] = Oot3dAotAsr(R[11], 7U);
            } else {
                // 0x004A042C
                if (!g.Enter()) { flow = g.BlockLimit(0x004A042CU); return Accept(kIdx); }
                R[14] = Read32(memory, R[0] + 0xBCU);
                R[11] = 0U - R[2];
                R[11] = R[14] * R[11];
                R[11] = Oot3dAotAsr(R[11], 7U);
                R[11] = 0U - R[11];
            }
            // 0x004A0450
            if (!g.Enter()) { flow = g.BlockLimit(0x004A0450U); return Accept(kIdx); }
            R[7] = R[7] + R[11];
            memory.WriteFast<uint32_t>(R[8] + (R[5] << 2U), R[7]);
            R[7] = Read32(memory, R[13]);
            R[11] = Read32(memory, R[9] + 0x78U);
            R[8] = R[7] - R[2];
            R[2] = Read32(memory, R[0] + 0xC8U);
            R[7] = Read32(memory, R[11] + (R[6] << 2U));
            R[14] = R[7];
            flags.SetSubtract(R[7], 0U, Mask(0x9U));
            if (flags.N() == flags.V()) {
                R[14] = R[14] * R[2];
                R[14] = Oot3dAotAsr(R[14], 7U);
            } else {
                // 0x004A0480
                if (!g.Enter()) { flow = g.BlockLimit(0x004A0480U); return Accept(kIdx); }
                R[14] = 0U - R[14];
                R[14] = R[14] * R[2];
                R[14] = Oot3dAotAsr(R[14], 7U);
                R[14] = 0U - R[14];
            }
            // 0x004A0490
            if (!g.Enter()) { flow = g.BlockLimit(0x004A0490U); return Accept(kIdx); }
            flags.SetAdd(R[8], R[14], Mask(0x8U));
            R[8] = R[8] + R[14];
            memory.WriteFast<uint32_t>(R[11] + (R[6] << 2U), R[8]);
            if (!flags.N()) {
                R[2] = R[8] * R[2];
                R[2] = Oot3dAotAsr(R[2], 7U);
            } else {
                // 0x004A04A4
                if (!g.Enter()) { flow = g.BlockLimit(0x004A04A4U); return Accept(kIdx); }
                R[8] = 0U - R[8];
                R[2] = R[8] * R[2];
                R[2] = Oot3dAotAsr(R[2], 7U);
                R[2] = 0U - R[2];
            }
            // 0x004A04B4
            if (!g.Enter()) { flow = g.BlockLimit(0x004A04B4U); return Accept(kIdx); }
            R[8] = Read32(memory, R[9] + 0xCCU);
            R[2] = R[7] - R[2];
            R[7] = Read32(memory, R[0] + 0xE8U);
            R[8] = R[8] + R[2];
            R[3] += 1U;
            R[12] += 1U;
            R[7] = R[7] * R[8];
            R[4] += 1U;
            R[5] += 1U;
            R[6] += 1U;
            R[2] = R[2] - Oot3dAotAsr(R[7], 7U);
            memory.WriteFast<uint32_t>(R[9] + 0xCCU, R[2]);
            R[8] = Read32(memory, R[0] + 0xE0U);
            R[7] = Read32(memory, R[13] + 0x4U);
            R[2] = R[2] * R[8] + R[7];
            R[7] = R[13] + 0x10U;
            R[7] = Read32(memory, R[7] + (R[10] << 2U));
            R[2] = Oot3dAotAsr(R[2], 7U);
            memory.WriteFast<uint32_t>(R[7] + (R[1] << 2U), R[2]);
            R[1] += 1U;
            flags.SetSubtract(R[1], 0x000000A0U, Mask(0x9U));
            if (flags.N() == flags.V()) break;
        }
        // 0x004A050C
        if (!g.Enter()) { flow = g.BlockLimit(0x004A050CU); return Accept(kIdx); }
        R[10] += 1U;
        flags.SetSubtract(R[10], 0x00000002U, Mask(0x9U));
        if (flags.N() == flags.V()) break;
    }
    // 0x004A0518: writes the circular indices (wrapping at the limit) and returns
    if (!g.Enter()) { flow = g.BlockLimit(0x004A0518U); return Accept(kIdx); }
    R[2] = Read32(memory, R[0] + 0x98U);
    R[1] = 0U;
    const auto wrap = [&](uint32_t limitOffset, uint32_t index, uint32_t fieldOffset,
                          uint8_t mask) {
        R[2] = Read32(memory, R[0] + limitOffset);
        flags.SetSubtract(R[2], index, Mask(mask));
        memory.WriteFast<uint32_t>(R[0] + fieldOffset,
                                   flags.C() && !flags.Z() ? index : R[1]);
    };
    flags.SetSubtract(R[2], R[3], Mask(0x6U));
    memory.WriteFast<uint32_t>(R[0] + 0x9CU, flags.C() && !flags.Z() ? R[3] : R[1]);
    wrap(0xA0U, R[12], 0xA4U, 0x6U);
    wrap(0xA8U, R[4], 0xB0U, 0x6U);
    wrap(0xACU, R[5], 0xB4U, 0x6U);
    wrap(0xC0U, R[6], 0xC4U, 0xFU);
    R[13] += 36U;
    uint32_t popped[9];
    for (uint32_t i = 0; i < 9U; ++i) popped[i] = Read32(memory, R[13] + 4U * i);
    for (uint32_t i = 0; i < 8U; ++i) R[4 + i] = popped[i];
    R[13] += 36U;
    g.Flush();
    flow = Oot3dAotReturned(popped[8]);
    return Accept(kIdx);
}

// ---------------------------------------------------------------------------
// 0x00371738 Runtime_Memcpy (leaf; word copy with src/dst aligned
// by the caller): push {r4-r10, lr}; 32-byte iterations (ldm/ldm/stm/stm,
// 1 block per iteration); then 16 and 8 bytes per bits 4 and 3 of r2, pop,
// 4 bytes (bit 2) and, if any remain, 2 and 1 bytes in block 0x00371794.
// The loop becomes per-page-chunk copies: the translation writes word by
// word in ascending address order, and the same bytes and written pages are
// reproduced chunk by chunk. Requires src and dst aligned to 4, no
// overlap between source and destination (each map region has its own
// bytes, so disjoint guest ranges are disjoint on the host) and everything
// in fast pages; the rest (head and tail) uses the same
// ReadWordsFast/WriteWordsFast/ReadFast/WriteFast calls as the translation.
bool Override_00371738(Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,
                       Oot3dAotArchitecturalState& state,
                       Oot3dWholeAotFlow& flow) noexcept {
    constexpr size_t kIdx = kIdx00371738;
    static_cast<void>(frame);
    auto& memory = context.Memory;
    auto& R = state.R;
    auto& flags = state.Flags;
    const uint32_t dst = R[0], src = R[1], len = R[2], sp = R[13];
    if (Hooked(context, 0x00371738U, 0x003717A8U)) return Decline(kIdx);
    const uint32_t loops = len >> 5U;
    // 0x00371738 [+ 0x00371744 + iterations at 0x00371750] + 0x0037176C [+ 0x00371794]
    const uint32_t blocks = 2U + (loops != 0U ? 1U + loops : 0U) + ((len & 3U) != 0U ? 1U : 0U);
    if (len > 0x00100000U || ((src | dst) & 3U) != 0U || !Budget(context, blocks) ||
        !Writable(memory, sp - 32U, 32U) || !Readable(memory, sp - 32U, 32U) ||
        !Readable(memory, src, len) || !Writable(memory, dst, len) ||
        Overlaps(src, len, dst, len)) {
        return Decline(kIdx);
    }
    Consume(context, blocks);
    // 0x00371738: push {r4-r10, lr}; subs r2, r2, #0x20
    {
        const uint32_t firstAddress = sp - 32U;
        const uint32_t words[8] = {R[4], R[5], R[6], R[7], R[8], R[9], R[10], R[14]};
        if (!memory.WriteWordsFast(firstAddress, words, 8U)) {
            for (uint32_t i = 0; i < 8U; ++i) memory.WriteFast<uint32_t>(firstAddress + 4U * i, words[i]);
        }
        R[13] = firstAddress;
    }
    flags.SetSubtract(R[2], 0x20U, Mask(0x3U));
    R[2] -= 0x20U;
    if (loops != 0U) {
        // 0x00371744 + 0x00371750 x iterations: copies 32 * iterations bytes.
        const uint32_t bytes = loops << 5U;
        uint32_t done = 0U;
        while (done < bytes) {
            const uint32_t from = src + done, to = dst + done;
            const uint32_t chunk = std::min({bytes - done, kMemPageSize - (from & kMemPageMask),
                                             kMemPageSize - (to & kMemPageMask)});
            auto* readPage = FastPageOf(memory, from, chunk);
            auto* writePage = FastPageOf(memory, to, chunk);
            std::memcpy(writePage->Write + (to & kMemPageMask),
                        readPage->Bytes + (from & kMemPageMask), chunk);
            memory.NoteWrite(to, chunk);
            done += chunk;
        }
        // Registers of the last iteration (r3-r10 = the final 8 words).
        const uint32_t last = src + bytes - 32U;
        for (uint32_t i = 0; i < 8U; ++i) R[3 + i] = Read32(memory, last + 4U * i);
        R[0] = dst + bytes;
        R[1] = src + bytes;
        flags.SetSubtract(len - bytes, 0x20U, Mask(0x3U));
        R[2] = len - bytes - 0x20U;
    }
    // 0x0037176C
    {
        const auto shifted = Oot3dAotShiftImmediate(R[2], 0U, 28U, flags.C());
        R[12] = shifted.Value;
        Oot3dAotSetLogicalFlags(flags, R[12], shifted.Carry, Mask(0xAU));
    }
    if (flags.C()) {
        uint32_t words[4];
        if (!memory.ReadWordsFast(R[1], words, 4U)) {
            for (uint32_t i = 0; i < 4U; ++i) memory.ReadFast<uint32_t>(R[1] + 4U * i, &words[i]);
        }
        R[3] = words[0];
        R[4] = words[1];
        R[12] = words[2];
        R[14] = words[3];
        R[1] += 16U;
        const uint32_t out[4] = {R[3], R[4], R[12], R[14]};
        if (!memory.WriteWordsFast(R[0], out, 4U)) {
            for (uint32_t i = 0; i < 4U; ++i) memory.WriteFast<uint32_t>(R[0] + 4U * i, out[i]);
        }
        R[0] += 16U;
    }
    if (flags.N()) {
        uint32_t words[2];
        if (!memory.ReadWordsFast(R[1], words, 2U)) {
            for (uint32_t i = 0; i < 2U; ++i) memory.ReadFast<uint32_t>(R[1] + 4U * i, &words[i]);
        }
        R[3] = words[0];
        R[4] = words[1];
        R[1] += 8U;
        if (!memory.WriteWordsFast(R[0], words, 2U)) {
            for (uint32_t i = 0; i < 2U; ++i) memory.WriteFast<uint32_t>(R[0] + 4U * i, words[i]);
        }
        R[0] += 8U;
    }
    {
        uint32_t words[8];
        if (!memory.ReadWordsFast(R[13], words, 8U)) {
            for (uint32_t i = 0; i < 8U; ++i) memory.ReadFast<uint32_t>(R[13] + 4U * i, &words[i]);
        }
        for (uint32_t i = 0; i < 7U; ++i) R[4 + i] = words[i];
        R[14] = words[7];
        R[13] += 32U;
    }
    {
        const auto shifted = Oot3dAotShiftImmediate(R[2], 0U, 30U, flags.C());
        R[12] = shifted.Value;
        Oot3dAotSetLogicalFlags(flags, R[12], shifted.Carry, Mask(0xEU));
    }
    if (flags.C()) {
        R[3] = Read32(memory, R[1]);
        R[1] += 4U;
        memory.WriteFast<uint32_t>(R[0], R[3]);
        R[0] += 4U;
    }
    if (flags.Z()) {
        flow = Oot3dAotReturned(R[14]);
        return Accept(kIdx);
    }
    // 0x00371794
    {
        const auto shifted = Oot3dAotShiftImmediate(R[2], 0U, 31U, flags.C());
        R[2] = shifted.Value;
        Oot3dAotSetLogicalFlags(flags, R[2], shifted.Carry, Mask(0xEU));
    }
    if (flags.C()) {
        uint16_t value = 0U;
        memory.ReadFast<uint16_t>(R[1], &value);
        R[3] = value;
        R[1] += 2U;
    }
    if (flags.N()) {
        uint8_t value = 0U;
        memory.ReadFast<uint8_t>(R[1], &value);
        R[2] = value;
        R[1] += 1U;
    }
    if (flags.C()) {
        memory.WriteFast<uint16_t>(R[0], static_cast<uint16_t>(R[3]));
        R[0] += 2U;
    }
    if (flags.N()) {
        memory.WriteFast<uint8_t>(R[0], static_cast<uint8_t>(R[2]));
        R[0] += 1U;
    }
    flow = Oot3dAotReturned(R[14]);
    return Accept(kIdx);
}

// ---------------------------------------------------------------------------
// 0x002DCEF4 Mtx3x3_Multiply (leaf, 2 blocks): r3 = 0; r1 += r3; r2 += r3;
// r3 = 12; cmp r3, #12 (always equal: the generic path 0x002DCFB4 never runs
// from the entry) and block 0x002DCF0C:
//   vpush {d8}; vldmia r2!, {s10-s15}; 9 VMUL and 18 VMLA (unfused) with
//   s16/s17 reloaded from r1; vldmia r2, {s10-s12}; vpop {d8};
//   vstmia r0, {s0-s8}; bx lr
// Memory accesses are the same ReadFast/WriteFast calls, in the same
// order (the reads of r1/r2 come after the vpush, so they see what it
// wrote if they overlap the stack). The VFP operations are the translation's,
// with exact zeros on the native path (Oot3dVfpZeroAware*: same value and
// same flags).
bool Override_002DCEF4(Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,
                       Oot3dAotArchitecturalState& state,
                       Oot3dWholeAotFlow& flow) noexcept {
    constexpr size_t kIdx = kIdx002DCEF4;
    auto& guest = frame.Guest;
    auto& memory = context.Memory;
    auto& R = state.R;
    const uint32_t r0 = R[0], r1 = R[1], r2 = R[2], sp = R[13];
    if (!Budget(context, 2U) || Hooked(context, 0x002DCEF4U, 0x002DCFB0U) ||
        !VfpDefault(guest.fpscr) || !Writable(memory, sp - 8U, 8U) ||
        !Readable(memory, sp - 8U, 8U) || !Readable(memory, r2, 36U) ||
        !Readable(memory, r1, 36U) || !Writable(memory, r0, 36U)) {
        return Decline(kIdx);
    }
    auto& v = guest.vfp;
    uint32_t fpscr = guest.fpscr;
    // vpush {d8}
    memory.WriteFast<uint32_t>(sp - 8U, v[16]);
    memory.WriteFast<uint32_t>(sp - 4U, v[17]);
    // vldmia r2!, {s10-s15}
    uint32_t b[9];
    for (uint32_t i = 0; i < 6U; ++i) b[i] = Read32(memory, r2 + 4U * i);
    uint32_t a[9];
    a[0] = Read32(memory, r1);
    a[3] = Read32(memory, r1 + 12U);
    a[6] = Read32(memory, r1 + 24U);
    a[1] = Read32(memory, r1 + 4U);
    a[4] = Read32(memory, r1 + 16U);
    for (uint32_t i = 6U; i < 9U; ++i) b[i] = Read32(memory, r2 + 24U + 4U * (i - 6U));
    a[7] = Read32(memory, r1 + 28U);
    a[2] = Read32(memory, r1 + 8U);
    a[5] = Read32(memory, r1 + 20U);
    a[8] = Read32(memory, r1 + 32U);
    // s[3r + c] = b[c] * a[r] + b[3 + c] * a[r + 3] + b[6 + c] * a[r + 6]
    // (in this summation order; the translation interleaves the rows, which changes
    // nothing: each output is an independent chain and the flags accumulate by OR).
    uint32_t out[9];
    for (uint32_t row = 0; row < 3U; ++row) {
        for (uint32_t column = 0; column < 3U; ++column) {
            uint32_t& value = out[3U * row + column];
            Oot3dVfpZeroAwareMultiply(value, b[column], a[3U * row], fpscr);
            Oot3dVfpZeroAwareMultiplyAccumulate(value, value, b[3U + column], a[3U * row + 1U], fpscr);
            Oot3dVfpZeroAwareMultiplyAccumulate(value, value, b[6U + column], a[3U * row + 2U], fpscr);
        }
    }
    // vpop {d8}; vstmia r0, {s0-s8}
    const uint32_t s16 = Read32(memory, sp - 8U);
    const uint32_t s17 = Read32(memory, sp - 4U);
    for (uint32_t i = 0; i < 9U; ++i) memory.WriteFast<uint32_t>(r0 + 4U * i, out[i]);
    for (uint32_t i = 0; i < 9U; ++i) v[i] = out[i];
    for (uint32_t i = 0; i < 6U; ++i) v[10U + i] = b[(i < 3U ? 6U : 0U) + i];
    v[16] = s16;
    v[17] = s17;
    guest.fpscr = fpscr;
    R[2] = r2 + 24U;
    R[3] = 12U;
    Oot3dAotSetSubtractFlags(state.Flags, 12U, 12U, Mask(0xFU));
    Consume(context, 2U);
    flow = Oot3dAotReturned(R[14]);
    return Accept(kIdx);
}

// ===========================================================================
// Block replacements (see oot3d_native_guest_overrides.h).

// ---------------------------------------------------------------------------
// 0x00466E68 MeshCommandPacket_Submit, copies the packet into the command
// list: block 0x00466E68 + n iterations of 0x00466E7C, resumes at 0x00466E94.
//   r0 = r1 - 4; r1 = [sp] - 4; r3 = [r0 + 4]; r2 = asr(r2, 3) (= n)
//   iteration: ip = [r0 += 8]; [r1 + 4] = r3; r3 = [r0 + 4]; subs r2, #1;
//          [r1 += 8] = ip; bne
// Reads [src, src + 8n + 4) (the last word is the read-ahead) and writes
// [dst, dst + 8n) in ascending words.
bool Override_00466E68(Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,
                       Oot3dAotArchitecturalState& state, Oot3dWholeAotFlow& flow,
                       uint32_t& resumePc) noexcept {
    constexpr size_t kIdx = kIdx00466E68;
    static_cast<void>(frame);
    static_cast<void>(flow);
    auto& memory = context.Memory;
    auto& R = state.R;
    const uint32_t src = R[1], sp = R[13];
    const uint32_t count = static_cast<uint32_t>(static_cast<int32_t>(R[2]) >> 3);
    // n <= 0 would give (almost) 2^32 iterations: leave it to the translation.
    if (Hooked(context, 0x00466E68U, 0x00466E90U) || static_cast<int32_t>(count) <= 0 ||
        count > 0x00020000U || !Budget(context, 1U + count) ||
        !SinglePageReadable(memory, sp, 4U)) {
        return Decline(kIdx);
    }
    const uint32_t dst = Read32(memory, sp);
    const uint32_t bytes = 8U * count;
    if (!Readable(memory, src, bytes + 4U) || !Writable(memory, dst, bytes) ||
        Overlaps(src, bytes + 4U, dst, bytes)) {
        return Decline(kIdx);
    }
    Consume(context, 1U + count);
    CopyRun(memory, dst, src, bytes);
    R[0] = src - 4U + bytes;
    R[1] = dst - 4U + bytes;
    R[2] = 0U;
    R[3] = Read32(memory, src + bytes);
    R[12] = Read32(memory, src + bytes - 4U);
    Oot3dAotSetSubtractFlags(state.Flags, 1U, 1U, Mask(0xFU));
    resumePc = 0x00466E94U;
    return Accept(kIdx);
}

// ---------------------------------------------------------------------------
// 0x003247B0 BgCheck_LineTestImpl, loop that clears the map of already-tested
// polygons (one block per byte), resumes at 0x003247C4:
//   strb r5, [r0], #1; r2 = [r8 + 0x4C] + r1; cmp r2, r0; bhi (loop)
// The limit is reread every iteration; if the loop itself wrote over
// [r8 + 0x4C] the limit would change, so the replacement declines.
bool Override_003247B0(Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,
                       Oot3dAotArchitecturalState& state, Oot3dWholeAotFlow& flow,
                       uint32_t& resumePc) noexcept {
    constexpr size_t kIdx = kIdx003247B0;
    static_cast<void>(frame);
    static_cast<void>(flow);
    auto& memory = context.Memory;
    auto& R = state.R;
    const uint32_t start = R[0];
    const uint32_t baseAddress = R[8] + 0x4CU;
    if (Hooked(context, 0x003247B0U, 0x003247C0U) || start == 0xFFFFFFFFU ||
        !SinglePageReadable(memory, baseAddress, 4U)) {
        return Decline(kIdx);
    }
    const uint32_t limit = Read32(memory, baseAddress) + R[1];
    // After k iterations r0 = start + k; stops at the first where limit <= r0.
    const uint32_t count = limit > start + 1U ? limit - start : 1U;
    // If the budget runs out first, do the iterations that fit and exit via the limit
    // at the loop top, like the translation (the prologue sits at the loop top; a
    // decline here would repeat on every translated iteration).
    const uint32_t done = std::min(count, context.BlocksRemaining);
    if (count > 0x00100000U || done == 0U ||
        !Writable(memory, start, done, 1U) || Overlaps(start, done, baseAddress, 4U)) {
        return Decline(kIdx);
    }
    Consume(context, done);
    FillBytes(memory, start, static_cast<uint8_t>(R[5]), done);
    R[0] = start + done;
    R[2] = limit;
    Oot3dAotSetSubtractFlags(state.Flags, limit, start + done, Mask(0xFU));
    if (done < count) {
        flow = Oot3dAotBlockLimit(context, 0x003247B0U);
        resumePc = 0U;
    } else {
        resumePc = 0x003247C4U;
    }
    return Accept(kIdx);
}

// ---------------------------------------------------------------------------
// 0x0047AF24 (leaf; search over 50 entries): r1 = u16 flag table
// (+0x151C), r0 = object (r14 = r0 + 0xA98: u16 flags at +0x156C and
// 36-byte pointers at +0x54), r2 = value sought. For each i < 50 with
// bit 0 of flags1[i]: ptr = (flags2[i] has bit 0 and not bit 1)
// ? entry[i].ptr : 0; if ptr != 0 and ptr == r2, pops and falls into the tail
// 0x00483C9C ([r2 + 0x1B8] = 0, r1 = 0). Not found: pop {r4, pc} (lr is not
// restored: r14 exits = r0 + 0xA98).
// The reads must not land on the pushed stack (else they would read r4/lr):
// then it declines. Everything is simulated in locals and only then written.
bool Override_0047AF24(Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,
                       Oot3dAotArchitecturalState& state,
                       Oot3dWholeAotFlow& flow) noexcept {
    constexpr size_t kIdx = kIdx0047AF24;
    static_cast<void>(frame);
    auto& memory = context.Memory;
    auto& R = state.R;
    const uint32_t sp = R[13];
    if (Hooked(context, 0x0047AF24U, 0x0047AFACU) || Hooked(context, 0x00483C9CU, 0x00483C9CU) ||
        !Writable(memory, sp - 8U, 8U)) {
        return Decline(kIdx);
    }
    const auto readable = [&](uint32_t address, uint32_t size) {
        return SinglePageReadable(memory, address, size) && !Overlaps(address, size, sp - 8U, 8U);
    };
    Oot3dAotLazyFlags flags = state.Flags;
    uint32_t r0 = R[2], r1 = R[1], r2 = R[2], r3 = 0U, r12 = R[0];
    uint32_t r14 = r12 + 0x00000800U;
    r14 += 0x00000298U;
    uint32_t blocks = 1U; // 0x0047AF24
    bool tail = false;
    while (true) {
        ++blocks; // 0x0047AF3C
        {
            const uint32_t address = r1 + (r3 << 1U) + 0x00001500U + 0x1CU;
            if (!readable(address, 2U)) return Decline(kIdx);
            uint16_t value = 0U;
            memory.ReadFast<uint16_t>(address, &value);
            r2 = value;
            Oot3dAotSetLogicalFlags(flags, r2 & 0x00000001U, flags.C(), Mask(0x4U));
        }
        if (!flags.Z()) {
            ++blocks; // 0x0047AF50
            Oot3dAotSetSubtractFlags(flags, r3, 0x00000032U, Mask(0x2U));
            bool clear = true; // -> 0x0047AF74
            if (!flags.C()) {
                ++blocks; // 0x0047AF58
                r2 = r14 + (r3 << 1U) + 0x00001500U;
                const uint32_t address = r2 + 0x6CU;
                if (!readable(address, 2U)) return Decline(kIdx);
                uint16_t value = 0U;
                memory.ReadFast<uint16_t>(address, &value);
                r12 = value;
                Oot3dAotSetLogicalFlags(flags, r12 & 0x00000001U, flags.C(), Mask(0x6U));
                if (!flags.Z()) {
                    ++blocks; // 0x0047AF6C
                    Oot3dAotSetLogicalFlags(flags, r12 & 0x00000002U, flags.C(), Mask(0x4U));
                    if (flags.Z()) {
                        ++blocks; // 0x0047AF7C
                        clear = false;
                        r12 = r3 + (r3 << 3U);
                        r12 = r12 + (r12 << 1U);
                        r2 = r14 + (r12 << 2U);
                        const uint32_t pointer = r2 + 0x54U;
                        if (!readable(pointer, 4U)) return Decline(kIdx);
                        r12 = Read32(memory, pointer);
                    }
                }
            }
            if (clear) {
                ++blocks; // 0x0047AF74
                r12 = 0x00000000U;
            }
            ++blocks; // 0x0047AF8C
            Oot3dAotSetSubtractFlags(flags, r12, 0x00000000U, Mask(0x4U));
            if (!flags.Z()) {
                ++blocks; // 0x0047AF94
                Oot3dAotSetSubtractFlags(flags, r12, r0, Mask(0xFU));
                if (flags.Z()) {
                    tail = true;
                    break;
                }
            }
        }
        ++blocks; // 0x0047AFA0
        r3 += 0x00000001U;
        Oot3dAotSetSubtractFlags(flags, r3, 0x00000032U, Mask(0xFU));
        if (flags.N() == flags.V()) break;
    }
    ++blocks; // 0x00483C9C or 0x0047AFAC
    if (!Budget(context, blocks) ||
        (tail && !Writable(memory, r0 + 0x1B8U, 1U, 1U))) {
        return Decline(kIdx);
    }
    // From here on there is no more declining.
    const uint32_t pushed[2] = {R[4], R[14]};
    StoreWords(memory, sp - 8U, pushed, 2U);
    Consume(context, blocks);
    R[0] = r0;
    R[2] = r2;
    R[3] = r3;
    R[12] = r12;
    R[13] = sp; // R[4] returns to the pushed value (the same)
    state.Flags = flags;
    if (tail) {
        R[1] = 0x00000000U;
        R[14] = pushed[1];
        memory.WriteFast<uint8_t>(r0 + 0x1B8U, 0U);
    } else {
        R[1] = r1;
        R[14] = r14;
    }
    flow = Oot3dAotReturned(pushed[1]);
    return Accept(kIdx);
}

// ---------------------------------------------------------------------------
// 0x00307B50 PicaTev_ConvertSource (leaf): copies the 16-word table from
// 0x004EA254 to the stack and searches for r0 in pairs (indices 0..63, reading past
// the copy: pushed r4/r5 and the caller's frame, like the original);
// returns the index or -1.
bool Override_00307B50(Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,
                       Oot3dAotArchitecturalState& state,
                       Oot3dWholeAotFlow& flow) noexcept {
    constexpr size_t kIdx = kIdx00307B50;
    constexpr uint32_t kTable = 0x004EA254U; // literal at 0x00307BD4
    static_cast<void>(frame);
    auto& memory = context.Memory;
    auto& R = state.R;
    const uint32_t sp = R[13], key = R[0];
    const uint32_t copy = sp - 0x48U;
    if (Hooked(context, 0x00307B50U, 0x00307BC4U) || !Writable(memory, copy, 0x48U) ||
        !Readable(memory, kTable, 0x40U) || Overlaps(kTable, 0x40U, copy, 0x48U)) {
        return Decline(kIdx);
    }
    uint32_t table[16];
    for (uint32_t i = 0; i < 16U; ++i) table[i] = Read32(memory, kTable + 4U * i);
    // Stack word i after the copy (nothing above sp is written).
    const auto word = [&](uint32_t i, uint32_t& value) {
        if (i < 16U) { value = table[i]; return true; }
        if (i < 18U) { value = i == 16U ? R[4] : R[5]; return true; }
        const uint32_t address = sp + 4U * (i - 18U);
        if (!SinglePageReadable(memory, address, 4U)) return false;
        value = Read32(memory, address);
        return true;
    };
    Oot3dAotLazyFlags flags = state.Flags;
    uint32_t r0 = 0U, r1 = 0U;
    uint32_t blocks = 1U; // 0x00307B50
    bool found = false;
    while (true) {
        ++blocks; // 0x00307B8C
        if (!word(r0, r1)) return Decline(kIdx);
        Oot3dAotSetSubtractFlags(flags, r1, key, Mask(0xFU));
        if (flags.Z()) { found = true; break; }
        ++blocks; // 0x00307B98
        if (!word(r0 + 1U, r1)) return Decline(kIdx);
        Oot3dAotSetSubtractFlags(flags, r1, key, Mask(0xFU));
        if (flags.Z()) {
            ++blocks; // 0x00307BA8
            r0 += 1U;
            found = true;
            break;
        }
        ++blocks; // 0x00307BB8
        r0 += 2U;
        Oot3dAotSetSubtractFlags(flags, r0, 0x00000040U, Mask(0xFU));
        if (flags.N() == flags.V()) break;
    }
    ++blocks; // 0x00307BAC or 0x00307BC4
    if (!Budget(context, blocks)) return Decline(kIdx);
    const uint32_t pushed[2] = {R[4], R[5]};
    StoreWords(memory, sp - 8U, pushed, 2U);
    for (uint32_t i = 0; i < 16U; i += 4U) StoreWords(memory, copy + 4U * i, table + i, 4U);
    Consume(context, blocks);
    R[0] = found ? r0 : 0xFFFFFFFFU;
    R[1] = r1;
    R[2] = table[13];
    R[3] = table[14];
    R[12] = table[15];
    R[13] = sp;
    state.Flags = flags;
    flow = Oot3dAotReturned(R[14]);
    return Accept(kIdx);
}

// ---------------------------------------------------------------------------
// 0x00307DC8 PicaCompare_ConvertFunction (leaf): the same with the table of 8
// halfwords at 0x004EA36C (indices 0..15) and result 0 if not found.
bool Override_00307DC8(Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,
                       Oot3dAotArchitecturalState& state,
                       Oot3dWholeAotFlow& flow) noexcept {
    constexpr size_t kIdx = kIdx00307DC8;
    constexpr uint32_t kTable = 0x004EA36CU; // literal at 0x00307E30
    static_cast<void>(frame);
    auto& memory = context.Memory;
    auto& R = state.R;
    const uint32_t sp = R[13], key = R[0];
    const uint32_t copy = sp - 0x18U;
    if (Hooked(context, 0x00307DC8U, 0x00307E20U) || !Writable(memory, copy, 0x18U) ||
        !Readable(memory, kTable, 0x10U) || Overlaps(kTable, 0x10U, copy, 0x18U)) {
        return Decline(kIdx);
    }
    uint32_t table[4];
    for (uint32_t i = 0; i < 4U; ++i) table[i] = Read32(memory, kTable + 4U * i);
    const uint32_t stacked[6] = {table[0], table[1], table[2], table[3], R[4], R[5]};
    const auto half = [&](uint32_t i, uint32_t& value) {
        if (i < 12U) {
            value = (stacked[i >> 1U] >> (16U * (i & 1U))) & 0xFFFFU;
            return true;
        }
        const uint32_t address = sp + 2U * (i - 12U);
        if (!SinglePageReadable(memory, address, 2U)) return false;
        uint16_t read = 0U;
        memory.ReadFast<uint16_t>(address, &read);
        value = read;
        return true;
    };
    Oot3dAotLazyFlags flags = state.Flags;
    uint32_t r0 = 0U, r1 = table[0], r2 = table[1];
    uint32_t blocks = 1U; // 0x00307DC8
    bool found = false;
    while (true) {
        ++blocks; // 0x00307DE8
        r1 = copy + (r0 << 1U);
        if (!half(r0, r2)) return Decline(kIdx);
        Oot3dAotSetSubtractFlags(flags, r2, key, Mask(0xFU));
        if (flags.Z()) { found = true; break; }
        ++blocks; // 0x00307DF8
        if (!half(r0 + 1U, r1)) return Decline(kIdx);
        Oot3dAotSetSubtractFlags(flags, r1, key, Mask(0xFU));
        if (flags.Z()) {
            ++blocks; // 0x00307E04
            r0 += 1U;
            found = true;
            break;
        }
        ++blocks; // 0x00307E14
        r0 += 2U;
        Oot3dAotSetSubtractFlags(flags, r0, 0x00000010U, Mask(0xFU));
        if (flags.N() == flags.V()) break;
    }
    ++blocks; // 0x00307E08 or 0x00307E20
    if (!Budget(context, blocks)) return Decline(kIdx);
    const uint32_t pushed[2] = {R[4], R[5]};
    StoreWords(memory, sp - 8U, pushed, 2U);
    StoreWords(memory, copy, table, 4U);
    Consume(context, blocks);
    R[0] = found ? r0 : 0x00000000U;
    R[1] = r1;
    R[2] = r2;
    R[3] = table[2];
    R[12] = table[3];
    R[13] = sp;
    state.Flags = flags;
    flow = Oot3dAotReturned(R[14]);
    return Accept(kIdx);
}

// ---------------------------------------------------------------------------
// 0x002FA674 CalculateGyroscopeAxisStatus, average over the sample ring
// (blocks 0x002FA674/680/688 per iteration), resumes at 0x002FA6A8:
//   r4 = ring[r12]; exits if r4 < r7 or r4 > r8; r5 += r4; ++[r2];
//   r12 = (r12 - 1) & 0xFF; continues while r12 != r9.
// The counter [r2] is reread and rewritten every iteration: if a ring read
// landed on it, decline; otherwise the simulation uses the initial value + iterations.
bool Override_002FA674(Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,
                       Oot3dAotArchitecturalState& state, Oot3dWholeAotFlow& flow,
                       uint32_t& resumePc) noexcept {
    constexpr size_t kIdx = kIdx002FA674;
    static_cast<void>(frame);
    static_cast<void>(flow);
    auto& memory = context.Memory;
    auto& R = state.R;
    const uint32_t counter = R[2], ring = R[6];
    // Block quota for the worst case (all iterations until r12 == r9), checked
    // before simulating: this prologue is at the loop top and, if it declines, runs
    // again on every translated iteration, so declining must be cheap.
    // r12/r9 come from "and #0xFF"; beyond that the loop only stops at the limits.
    uint32_t turns = (R[12] - R[9]) & 0xFFU;
    if (turns == 0U) turns = 256U;
    if (R[12] > 0xFFU || R[9] > 0xFFU || !Budget(context, 3U * turns) ||
        Hooked(context, 0x002FA674U, 0x002FA6A4U) || !SinglePageReadable(memory, counter, 4U) ||
        !SinglePageWritable(memory, counter, 4U)) {
        return Decline(kIdx);
    }
    Oot3dAotLazyFlags flags = state.Flags;
    uint32_t r4 = R[4], r5 = R[5], r12 = R[12];
    const uint32_t initial = Read32(memory, counter);
    uint32_t writes = 0U, blocks = 0U;
    while (true) {
        ++blocks; // 0x002FA674
        const uint32_t address = ring + (r12 << 2U);
        if (!SinglePageReadable(memory, address, 4U) || Overlaps(address, 4U, counter, 4U)) {
            return Decline(kIdx);
        }
        r4 = Read32(memory, address);
        Oot3dAotSetSubtractFlags(flags, r4, R[7], Mask(0x9U));
        if (flags.N() != flags.V()) break;
        ++blocks; // 0x002FA680
        Oot3dAotSetSubtractFlags(flags, r4, R[8], Mask(0xDU));
        if (!flags.Z() && flags.N() == flags.V()) break;
        ++blocks; // 0x002FA688
        r5 += r4;
        r4 = initial + writes;
        r12 = (r12 - 0x00000001U) & 0x000000FFU;
        r4 += 0x00000001U;
        Oot3dAotSetSubtractFlags(flags, r12, R[9], Mask(0x4U));
        ++writes;
        if (flags.Z()) break;
    }
    Consume(context, blocks);
    RepeatStore32(memory, counter, initial + writes, writes);
    R[4] = r4;
    R[5] = r5;
    R[12] = r12;
    state.Flags = flags;
    resumePc = 0x002FA6A8U;
    return Accept(kIdx);
}

// ---------------------------------------------------------------------------
// Animation curve search loops: find the first keyframe whose
// time (integer converted to float) is greater than s0/s1. Only registers,
// VFP and flags change; everything is simulated in locals with the same VFP operations
// as the generated code (Vfp::, Oot3dVfpNativeCompare) and written at the end. Declining after
// simulating is safe: nothing was written to guest state, and the accumulated flags
// that the console's native VFP has already raised in FPSR are the same ones the
// translation will raise when redoing the same operations on the same data.
// The prologue sits at the loop top; the block quota (worst case) is checked
// before simulating so declining is cheap.

// Worst-case iterations of a loop "do; ++i; continue while limit > i"
// (signed), starting from i: at least one; 0 if unbounded.
inline uint32_t SignedTurns(uint32_t limit, uint32_t index) noexcept {
    const int64_t span = int64_t{static_cast<int32_t>(limit)} - static_cast<int32_t>(index);
    if (span > 0x00010000) return 0U;
    return span > 1 ? static_cast<uint32_t>(span) : 1U;
}

// 0x00308920 NativeCurve_SampleFloat (r2 = keyframe, 16 bytes; r1 =
// count; r0 = index): r6 = [r2]; s1 = s0; s2 = (float)r6;
// exits if s2 > s1; else ++r0, r2 += 16, continues while r1 > r0.
// Resumes at 0x0030894C.
bool Override_00308920(Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,
                       Oot3dAotArchitecturalState& state, Oot3dWholeAotFlow& flow,
                       uint32_t& resumePc) noexcept {
    constexpr size_t kIdx = kIdx00308920;
    static_cast<void>(flow);
    auto& guest = frame.Guest;
    auto& memory = context.Memory;
    auto& R = state.R;
    const uint32_t turns = SignedTurns(R[1], R[0]);
    if (turns == 0U || !Budget(context, 2U * turns) || !VfpDefault(guest.fpscr) ||
        Hooked(context, 0x00308920U, 0x00308948U)) {
        return Decline(kIdx);
    }
    Oot3dAotLazyFlags flags = state.Flags;
    uint32_t fpscr = guest.fpscr;
    uint32_t s1 = guest.vfp[1], s2 = guest.vfp[2];
    const uint32_t s0 = guest.vfp[0], r1 = R[1];
    uint32_t r0 = R[0], r2 = R[2], r6 = R[6];
    uint32_t blocks = 0U;
    while (true) {
        ++blocks; // 0x00308920
        if (!SinglePageReadable(memory, r2, 4U)) return Decline(kIdx);
        r6 = Read32(memory, r2);
        s1 = s0;
        s2 = r6;
        Vfp::FromSigned(s2, s2, fpscr);
        Oot3dVfpNativeCompare(s2, s1, true, fpscr);
        flags.AssignPackedNzcv(fpscr);
        if (!flags.Z() && flags.N() == flags.V()) break;
        ++blocks; // 0x0030893C
        r0 += 0x00000001U;
        Oot3dAotSetSubtractFlags(flags, r1, r0, Mask(0xDU));
        r2 += 0x00000010U;
        if (!(!flags.Z() && flags.N() == flags.V())) break;
        if (blocks >= 2U * turns) return Decline(kIdx); // does not happen (quota above)
    }
    Consume(context, blocks);
    R[0] = r0;
    R[2] = r2;
    R[6] = r6;
    state.Flags = flags;
    guest.vfp[1] = s1;
    guest.vfp[2] = s2;
    guest.fpscr = fpscr;
    resumePc = 0x0030894CU;
    return Accept(kIdx);
}

// 0x00308604 CsabCurve_SampleRotation: the same with s16 times (ldrsh),
// 8-byte stride in r1, count in r2 and fixed s1. Resumes at 0x0030862C.
bool Override_00308604(Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,
                       Oot3dAotArchitecturalState& state, Oot3dWholeAotFlow& flow,
                       uint32_t& resumePc) noexcept {
    constexpr size_t kIdx = kIdx00308604;
    static_cast<void>(flow);
    auto& guest = frame.Guest;
    auto& memory = context.Memory;
    auto& R = state.R;
    const uint32_t turns = SignedTurns(R[2], R[0]);
    if (turns == 0U || !Budget(context, 2U * turns) || !VfpDefault(guest.fpscr) ||
        Hooked(context, 0x00308604U, 0x00308628U)) {
        return Decline(kIdx);
    }
    Oot3dAotLazyFlags flags = state.Flags;
    uint32_t fpscr = guest.fpscr;
    uint32_t s2 = guest.vfp[2];
    const uint32_t s1 = guest.vfp[1], r2 = R[2];
    uint32_t r0 = R[0], r1 = R[1], r6 = R[6];
    uint32_t blocks = 0U;
    while (true) {
        ++blocks; // 0x00308604
        if (!SinglePageReadable(memory, r1, 2U)) return Decline(kIdx);
        uint16_t value = 0U;
        memory.ReadFast<uint16_t>(r1, &value);
        r6 = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(value)));
        s2 = r6;
        Vfp::FromSigned(s2, s2, fpscr);
        Oot3dVfpNativeCompare(s2, s1, true, fpscr);
        flags.AssignPackedNzcv(fpscr);
        if (!flags.Z() && flags.N() == flags.V()) break;
        ++blocks; // 0x0030861C
        r0 += 0x00000001U;
        Oot3dAotSetSubtractFlags(flags, r2, r0, Mask(0xDU));
        r1 += 0x00000008U;
        if (!(!flags.Z() && flags.N() == flags.V())) break;
        if (blocks >= 2U * turns) return Decline(kIdx); // does not happen (quota above)
    }
    Consume(context, blocks);
    R[0] = r0;
    R[1] = r1;
    R[6] = r6;
    state.Flags = flags;
    guest.vfp[2] = s2;
    guest.fpscr = fpscr;
    resumePc = 0x0030862CU;
    return Accept(kIdx);
}

// ---------------------------------------------------------------------------
// 0x002BBB14 Math3D_CirSquareVsTriSquare (leaf, VFP only): bounding box of the three
// points (s0..s5: x0,y0,x1,y1,x2,y2) against the circle's square (s6, s7
// center, s8 radius). Block-by-block mirror of the translation, directly on guest
// state: after the entry check there is no declining and no memory access.
bool Override_002BBB14(Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,
                       Oot3dAotArchitecturalState& state,
                       Oot3dWholeAotFlow& flow) noexcept {
    constexpr size_t kIdx = kIdx002BBB14;
    auto& guest = frame.Guest;
    auto& s = guest.vfp;
    auto& fpscr = guest.fpscr;
    auto& flags = state.Flags;
    if (!Budget(context, 13U) || !VfpDefault(fpscr) ||
        Hooked(context, 0x002BBB14U, 0x002BBBC4U)) {
        return Decline(kIdx);
    }
    const auto compare = [&](uint32_t left, uint32_t right) {
        Oot3dVfpNativeCompare(left, right, true, fpscr);
        flags.AssignPackedNzcv(fpscr);
    };
    const auto greater = [&]() { return !flags.Z() && flags.N() == flags.V(); };
    uint32_t blocks = 1U; // 0x002BBB14
    s[9] = s[0];
    s[10] = s[1];
    compare(s[0], s[2]);
    if (greater()) {
        s[0] = s[2];
    } else {
        ++blocks; // 0x002BBB2C
        if (!flags.C()) s[9] = s[2];
    }
    ++blocks; // 0x002BBB30
    compare(s[1], s[3]);
    if (greater()) {
        s[1] = s[3];
    } else {
        ++blocks; // 0x002BBB40
        if (!flags.C()) s[10] = s[3];
    }
    ++blocks; // 0x002BBB44
    compare(s[0], s[4]);
    if (greater()) {
        s[0] = s[4];
    } else {
        ++blocks; // 0x002BBB54
        compare(s[9], s[4]);
        if (!flags.C()) s[9] = s[4];
    }
    ++blocks; // 0x002BBB60
    compare(s[1], s[5]);
    if (greater()) {
        s[1] = s[5];
    } else {
        ++blocks; // 0x002BBB70
        compare(s[10], s[5]);
        if (!flags.C()) s[10] = s[5];
    }
    bool inside = false;
    ++blocks; // 0x002BBB7C
    Vfp::Subtract(s[0], s[0], s[8], fpscr);
    compare(s[0], s[6]);
    if (!(flags.C() && !flags.Z())) {
        ++blocks; // 0x002BBB8C
        Vfp::Add(s[0], s[9], s[8], fpscr);
        compare(s[0], s[6]);
        if (flags.N() == flags.V()) {
            ++blocks; // 0x002BBB9C
            Vfp::Subtract(s[0], s[1], s[8], fpscr);
            compare(s[0], s[7]);
            if (!(flags.C() && !flags.Z())) {
                ++blocks; // 0x002BBBAC
                Vfp::Add(s[0], s[10], s[8], fpscr);
                compare(s[0], s[7]);
                inside = flags.N() == flags.V();
            }
        }
    }
    ++blocks; // 0x002BBBBC or 0x002BBBC4
    state.R[0] = inside ? 1U : 0U;
    Consume(context, blocks);
    flow = Oot3dAotReturned(state.R[14]);
    return Accept(kIdx);
}

// ---------------------------------------------------------------------------
// Short looped word copies (preheader + iterations), with the same
// ReadFast/WriteFast calls as the translation in the same order (all on the fast
// path, validated beforehand). Source and destination must not overlap (else a
// read would see an earlier write of the loop): then it declines.

// 0x00303C78 (FUN_00303b24): copies 8 word pairs from r5 + 4*r7 to
// r4 + 4*r7 (block 0x00303C78 + 8 iterations of 0x00303C8C), resumes at 0x00303CA4.
bool Override_00303C78(Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,
                       Oot3dAotArchitecturalState& state, Oot3dWholeAotFlow& flow,
                       uint32_t& resumePc) noexcept {
    constexpr size_t kIdx = kIdx00303C78;
    static_cast<void>(frame);
    static_cast<void>(flow);
    auto& memory = context.Memory;
    auto& R = state.R;
    const uint32_t src = R[5] + (R[7] << 2U), dst = R[4] + (R[7] << 2U);
    if (!Budget(context, 9U) || Hooked(context, 0x00303C78U, 0x00303CA0U) ||
        !Readable(memory, src, 64U) || !Writable(memory, dst, 64U) ||
        Overlaps(src, 64U, dst, 64U)) {
        return Decline(kIdx);
    }
    Consume(context, 9U);
    uint32_t r3 = 0U, r12 = 0U;
    for (uint32_t i = 0; i < 64U; i += 8U) {
        r12 = Read32(memory, src + i);
        memory.WriteFast<uint32_t>(dst + i, r12);
        r3 = Read32(memory, src + i + 4U);
        memory.WriteFast<uint32_t>(dst + i + 4U, r3);
    }
    R[0] = src - 4U + 64U;
    R[1] = dst - 4U + 64U;
    R[2] = 0U;
    R[3] = r3;
    R[12] = r12;
    Oot3dAotSetSubtractFlags(state.Flags, 1U, 1U, Mask(0x4U));
    resumePc = 0x00303CA4U;
    return Accept(kIdx);
}

// 0x0046700C MeshCommandPacket_PatchBoneUniforms: n = (r6 - 1) / 4 groups of
// 4 words from r4 copied to r0 with the order reversed within each group
// (block 0x0046700C + n iterations of 0x00467024), resumes at 0x0046705C.
bool Override_0046700C(Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,
                       Oot3dAotArchitecturalState& state, Oot3dWholeAotFlow& flow,
                       uint32_t& resumePc) noexcept {
    constexpr size_t kIdx = kIdx0046700C;
    static_cast<void>(flow);
    auto& memory = context.Memory;
    auto& R = state.R;
    const uint32_t r3 = R[6] - 1U;
    const uint32_t r12 = Oot3dAotAsr(r3, 31U);
    const uint32_t r9 = r3 + Oot3dAotLsr(r12, 30U);
    const uint32_t count = Oot3dAotAsr(r9, 2U);
    const uint32_t src = R[4], dst = R[0];
    // n <= 0 would give (almost) 2^32 iterations: leave it to the translation.
    if (static_cast<int32_t>(count) <= 0 || count > 0x1000U || !Budget(context, 1U + count) ||
        Hooked(context, 0x0046700CU, 0x00467058U) || !Readable(memory, src, 16U * count) ||
        !Writable(memory, dst, 16U * count) || Overlaps(src, 16U * count, dst, 16U * count)) {
        return Decline(kIdx);
    }
    Consume(context, 1U + count);
    uint32_t s0 = 0U;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t from = src + 16U * i + 12U, to = dst + 16U * i;
        for (uint32_t k = 0; k < 4U; ++k) {
            s0 = Read32(memory, from - 4U * k);
            memory.WriteFast<uint32_t>(to + 4U * k, s0);
        }
    }
    frame.Guest.vfp[0] = s0;
    R[1] = dst + 16U * count;
    R[2] = src + 12U + 16U * count;
    R[3] = 0U;
    R[5] = dst + 16U * (count - 1U);
    R[9] = r9;
    R[12] = src + 12U + 16U * (count - 1U);
    Oot3dAotSetSubtractFlags(state.Flags, 1U, 1U, Mask(0x6U));
    resumePc = 0x0046705CU;
    return Accept(kIdx);
}

// ---------------------------------------------------------------------------
// 0x003563E0 FUN_003562c0 (sorted insertion into the collision polygon
// list), walks the list to find the insertion point: reads only.
//   3563E0: r1 = next(r6) ([r6 + 2]); end of list (0xFFFF) -> 0x003563EC
//   35640C/438/454: polygon r4 = r7 + 4*r1, vertices from [r2 + 20*idx + 2..6]
//     (indices & 0x1FFF, y at [r3 + 6*v + 2]); if any y <= r12 -> 0x00356498
//     (advance: r6 = r4), else -> 0x0035646C (insert before it)
// Resumes at 0x003563EC or 0x0035646C. Since nothing is written, running out of
// block budget mid-walk is reproduced exactly (limit flow
// at the block that could not enter) instead of declining.
bool Override_003563E0(Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,
                       Oot3dAotArchitecturalState& state, Oot3dWholeAotFlow& flow,
                       uint32_t& resumePc) noexcept {
    constexpr size_t kIdx = kIdx003563E0;
    static_cast<void>(frame);
    auto& memory = context.Memory;
    auto& R = state.R;
    if (Hooked(context, 0x003563E0U, 0x00356498U)) return Decline(kIdx);
    Oot3dAotLazyFlags flags = state.Flags;
    uint32_t r1 = R[1], r4 = R[4], r5 = R[5], r6 = R[6];
    const uint32_t r2 = R[2], r3 = R[3], r7 = R[7], r8 = R[8], r12 = R[12];
    const uint32_t remaining = context.BlocksRemaining;
    uint32_t blocks = 0U, limitPc = 0U, exitPc = 0U;
    const auto enter = [&](uint32_t pc) {
        if (blocks == remaining) {
            limitPc = pc;
            return false;
        }
        ++blocks;
        return true;
    };
    const auto read16 = [&](uint32_t address, uint32_t& value, bool sign) {
        if (!SinglePageReadable(memory, address, 2U)) return false;
        uint16_t raw = 0U;
        memory.ReadFast<uint16_t>(address, &raw);
        value = sign ? static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw))) : raw;
        return true;
    };
    const auto vertexY = [&](uint32_t index, uint32_t& value) {
        index = index + (index << 1U);
        return read16(r3 + (index << 1U) + 2U, value, true);
    };
    const auto lowerOrEqual = [&]() { return flags.Z() || flags.N() != flags.V(); };
    while (true) {
        if (!enter(0x003563E0U)) break;
        if (!read16(r6 + 2U, r1, false)) return Decline(kIdx);
        Oot3dAotSetSubtractFlags(flags, r1, r8, Mask(0xFU));
        if (flags.Z()) {
            exitPc = 0x003563ECU;
            break;
        }
        bool advance = true;
        if (!enter(0x0035640CU)) break;
        r4 = r7 + (r1 << 2U);
        if (!read16(r4, r1, true)) return Decline(kIdx);
        r1 = r1 + (r1 << 2U);
        r1 = r2 + (r1 << 2U);
        if (!read16(r1 + 2U, r5, false)) return Decline(kIdx);
        r5 &= ~0x0000E000U;
        if (!vertexY(r5, r5)) return Decline(kIdx);
        Oot3dAotSetSubtractFlags(flags, r5, r12, Mask(0xDU));
        if (!lowerOrEqual()) {
            if (!enter(0x00356438U)) break;
            if (!read16(r1 + 4U, r5, false)) return Decline(kIdx);
            r5 &= ~0x0000E000U;
            if (!vertexY(r5, r5)) return Decline(kIdx);
            Oot3dAotSetSubtractFlags(flags, r5, r12, Mask(0xDU));
            if (!lowerOrEqual()) {
                if (!enter(0x00356454U)) break;
                if (!read16(r1 + 6U, r1, false)) return Decline(kIdx);
                if (!vertexY(r1, r1)) return Decline(kIdx);
                Oot3dAotSetSubtractFlags(flags, r1, r12, Mask(0xFU));
                advance = lowerOrEqual();
            }
        }
        if (!advance) {
            exitPc = 0x0035646CU;
            break;
        }
        if (!enter(0x00356498U)) break;
        r6 = r4; // a cyclic list terminates via the budget, like the translation
    }
    Consume(context, blocks);
    R[1] = r1;
    R[4] = r4;
    R[5] = r5;
    R[6] = r6;
    state.Flags = flags;
    if (limitPc != 0U) {
        flow = Oot3dAotBlockLimit(context, limitPc);
        resumePc = 0U;
    } else {
        resumePc = exitPc;
    }
    return Accept(kIdx);
}

} // namespace GuestOverrides
} // namespace Oot3dNativeGame
