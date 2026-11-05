// Captura PNG do quadro apresentado (--screenshot quadro.png).
//
// O host só gravava BMP (RGBA5551 nas capturas determinísticas). Para ver a
// primeira imagem de outro título (Super Mario 3D Land) o executável com
// janela grava PNG RGB de 8 bits sem dependência externa; este teste
// decodifica o PNG gerado (zlib "stored", CRC e Adler-32) e compara os pixels.
#include "oot3d_demo_host_io.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

uint32_t Be32(const std::vector<uint8_t>& data, size_t offset) {
    return (uint32_t{data[offset]} << 24) | (uint32_t{data[offset + 1]} << 16) |
           (uint32_t{data[offset + 2]} << 8) | data[offset + 3];
}

uint32_t Crc32(const uint8_t* data, size_t size) {
    uint32_t crc = 0xffffffffu;
    for (size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xffffffffu;
}

void TestRoundTrip(uint32_t width, uint32_t height) {
    std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3u);
    for (size_t index = 0; index < rgb.size(); ++index) rgb[index] = static_cast<uint8_t>(index * 37u + 11u);
    const auto png = EncodePngFromRgb8(width, height, rgb);
    Require(png.size() > 8 && png[0] == 0x89 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G',
            "PNG signature");
    // Percorre os chunks e confere o CRC de cada um.
    std::vector<uint8_t> zlib;
    size_t offset = 8;
    bool header = false, end = false;
    while (offset + 12 <= png.size()) {
        const uint32_t length = Be32(png, offset);
        const uint8_t* type = png.data() + offset + 4;
        Require(offset + 12 + length <= png.size(), "chunk past the end");
        Require(Crc32(type, length + 4u) == Be32(png, offset + 8 + length), "chunk CRC");
        const std::string name(reinterpret_cast<const char*>(type), 4);
        if (name == "IHDR") {
            header = Be32(png, offset + 8) == width && Be32(png, offset + 12) == height &&
                     png[offset + 16] == 8 && png[offset + 17] == 2;
        } else if (name == "IDAT") {
            zlib.insert(zlib.end(), type + 4, type + 4 + length);
        } else if (name == "IEND") {
            end = true;
        }
        offset += 12u + length;
    }
    Require(header && end && offset == png.size(), "IHDR/IEND");
    // zlib: blocos stored.
    Require(zlib.size() >= 6 && zlib[0] == 0x78 && ((zlib[0] << 8) | zlib[1]) % 31 == 0, "zlib header");
    std::vector<uint8_t> raw;
    size_t at = 2;
    for (;;) {
        Require(at + 5 <= zlib.size(), "stored block header");
        const bool last = (zlib[at] & 1u) != 0;
        Require((zlib[at] & 6u) == 0, "only stored blocks");
        const uint16_t len = static_cast<uint16_t>(zlib[at + 1] | (zlib[at + 2] << 8));
        const uint16_t nlen = static_cast<uint16_t>(zlib[at + 3] | (zlib[at + 4] << 8));
        Require(static_cast<uint16_t>(~len) == nlen, "stored block NLEN");
        raw.insert(raw.end(), zlib.begin() + static_cast<std::ptrdiff_t>(at + 5),
                   zlib.begin() + static_cast<std::ptrdiff_t>(at + 5 + len));
        at += 5u + len;
        if (last) break;
    }
    uint32_t a = 1, b = 0;
    for (const uint8_t value : raw) { a = (a + value) % 65521u; b = (b + a) % 65521u; }
    Require(at + 4 == zlib.size() && Be32(zlib, at) == ((b << 16) | a), "Adler-32");
    Require(raw.size() == (static_cast<size_t>(width) * 3u + 1u) * height, "raw size");
    for (uint32_t y = 0; y < height; ++y) {
        const size_t row = y * (static_cast<size_t>(width) * 3u + 1u);
        Require(raw[row] == 0, "filter byte");
        for (size_t x = 0; x < static_cast<size_t>(width) * 3u; ++x)
            Require(raw[row + 1 + x] == rgb[y * static_cast<size_t>(width) * 3u + x], "pixel differs");
    }
}

} // namespace

int main() {
    TestRoundTrip(3, 2);
    // Mais de 65535 bytes crus: vários blocos stored.
    TestRoundTrip(400, 240);
    Require(EncodePngFromRgb8(0, 0, {}).empty(), "empty image must not encode");
    Require(ScreenshotPathIsPng("a/quadro.PNG") && !ScreenshotPathIsPng("a/quadro.bmp"),
            "PNG extension");
    std::printf("oot3d_demo_host_png_tests: ok\n");
    return 0;
}
