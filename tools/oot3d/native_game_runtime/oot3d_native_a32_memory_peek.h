#pragma once

// config/vertex_snapshot_fast (SWITCH_PORT.md "Vertex snapshot hit path"):
// NativeA32Memory::RangeWriteGeneration reads each page's card before taking
// its RangeWritten bit and skips the atomic and-fetch on a clean card (the
// same certificates; oot3d_native_a32_memory.cpp). Declared here and not in
// oot3d_native_a32_memory.h, which the whole-AOT product build hashes and the
// generated code includes. Set before a second core runs.

namespace Oot3dNativeGame {

void SetRangeWriteGenerationCardPeek(bool enabled) noexcept;

} // namespace Oot3dNativeGame
