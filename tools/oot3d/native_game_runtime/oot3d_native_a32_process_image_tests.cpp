// Faixa de texto do guest tirada do manifesto do processo (perfilador do guest).
#include "oot3d_native_a32_process_image.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

using Oot3dNativeGame::NativeA32GuestTextRange;
using Oot3dNativeGame::NativeA32ProcessImageManifest;
using Oot3dNativeGame::NativeA32ProcessImageSegment;

void Require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "oot3d_native_a32_process_image_tests: " << message << '\n';
        std::exit(1);
    }
}

NativeA32ProcessImageSegment Segment(std::string name, uint32_t address, size_t mapped, bool executable) {
    NativeA32ProcessImageSegment segment;
    segment.Name = std::move(name);
    segment.Address = address;
    segment.MappedSize = mapped;
    segment.Executable = executable;
    return segment;
}

// O perfilador usava a faixa fixa do OoT3D (0x3CA000 bytes); o texto do MM3D tem
// 0x51D000 mapeados (ExHeader: 1309 páginas) e os sítios do dekui ficam além do OoT3D.
void TextRangeComesFromTheExecutableSegment() {
    NativeA32ProcessImageManifest manifest;
    manifest.Segments = {Segment("rodata", 0x0061D000U, 0x2E000U, false),
                         Segment("text", 0x00100000U, 0x0051D000U, true),
                         Segment("data_bss", 0x0064B000U, 0x184000U, false)};
    const auto range = NativeA32GuestTextRange(manifest);
    Require(range.has_value(), "sem faixa com segmento executável");
    Require(range->first == 0x00100000U && range->second == 0x0051D000U, "faixa de texto errada");
}

void NoExecutableSegmentHasNoRange() {
    NativeA32ProcessImageManifest manifest;
    manifest.Segments = {Segment("data_bss", 0x0064B000U, 0x184000U, false)};
    Require(!NativeA32GuestTextRange(manifest).has_value(), "inventou faixa sem segmento executável");
}

} // namespace

int main() {
    TextRangeComesFromTheExecutableSegment();
    NoExecutableSegmentHasNoRange();
    std::cout << "oot3d_native_a32_process_image_tests: 2/2\n";
    return 0;
}
