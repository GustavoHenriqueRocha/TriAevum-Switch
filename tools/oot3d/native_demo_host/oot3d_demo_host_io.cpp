#include "oot3d_demo_host_io.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

void WriteJsonFile(const std::filesystem::path& path, const nlohmann::json& data) {
    if (path.empty()) {
        return;
    }
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }
    std::ofstream file(path);
    if (!file) {
        throw std::runtime_error("could not write JSON file: " + path.string());
    }
    file << data.dump(2) << "\n";
}

namespace {

void WriteLe16(std::ofstream& file, uint16_t value) {
    const char bytes[2] = {
        static_cast<char>(value & 0xff),
        static_cast<char>((value >> 8) & 0xff),
    };
    file.write(bytes, sizeof(bytes));
}

void WriteLe32(std::ofstream& file, uint32_t value) {
    const char bytes[4] = {
        static_cast<char>(value & 0xff),
        static_cast<char>((value >> 8) & 0xff),
        static_cast<char>((value >> 16) & 0xff),
        static_cast<char>((value >> 24) & 0xff),
    };
    file.write(bytes, sizeof(bytes));
}

uint8_t ExpandRgba5551Channel(uint16_t value) {
    const uint8_t v = static_cast<uint8_t>(value & 0x1f);
    return static_cast<uint8_t>((v << 3) | (v >> 2));
}

void AppendBe32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value >> 24));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

uint32_t PngCrc32(const uint8_t* data, size_t size, uint32_t crc = 0xffffffffu) {
    for (size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

void AppendPngChunk(std::vector<uint8_t>& out, const char type[4], const std::vector<uint8_t>& data) {
    AppendBe32(out, static_cast<uint32_t>(data.size()));
    const size_t typeOffset = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    const uint32_t crc = PngCrc32(out.data() + typeOffset, data.size() + 4u) ^ 0xffffffffu;
    AppendBe32(out, crc);
}

} // namespace

bool ScreenshotPathIsPng(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".png";
}

std::vector<uint8_t> EncodePngFromRgb8(uint32_t width, uint32_t height, const std::vector<uint8_t>& rgb) {
    std::vector<uint8_t> png;
    if (width == 0 || height == 0 || rgb.size() < static_cast<size_t>(width) * height * 3u) {
        return png;
    }
    // Dados crus: cada linha com o filtro 0 (None) na frente.
    std::vector<uint8_t> raw;
    raw.reserve((static_cast<size_t>(width) * 3u + 1u) * height);
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);
        const auto* row = rgb.data() + static_cast<size_t>(y) * width * 3u;
        raw.insert(raw.end(), row, row + static_cast<size_t>(width) * 3u);
    }
    // zlib: cabeçalho, blocos stored de até 65535 bytes, Adler-32.
    std::vector<uint8_t> zlib{0x78, 0x01};
    size_t offset = 0;
    do {
        const size_t length = std::min<size_t>(65535u, raw.size() - offset);
        const bool last = offset + length == raw.size();
        zlib.push_back(last ? 1u : 0u);
        zlib.push_back(static_cast<uint8_t>(length));
        zlib.push_back(static_cast<uint8_t>(length >> 8));
        zlib.push_back(static_cast<uint8_t>(~length));
        zlib.push_back(static_cast<uint8_t>((~length) >> 8));
        zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset),
                    raw.begin() + static_cast<std::ptrdiff_t>(offset + length));
        offset += length;
    } while (offset < raw.size());
    uint32_t a = 1, b = 0;
    for (const uint8_t value : raw) {
        a = (a + value) % 65521u;
        b = (b + a) % 65521u;
    }
    AppendBe32(zlib, (b << 16) | a);

    static constexpr uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    png.assign(kSignature, kSignature + 8);
    std::vector<uint8_t> header;
    AppendBe32(header, width);
    AppendBe32(header, height);
    header.insert(header.end(), {8, 2, 0, 0, 0}); // 8 bits, RGB, deflate, filtro 0, sem entrelaçar
    AppendPngChunk(png, "IHDR", header);
    AppendPngChunk(png, "IDAT", zlib);
    AppendPngChunk(png, "IEND", {});
    return png;
}

void WritePngFromRgb8Framebuffer(
    const std::filesystem::path& path,
    uint32_t width,
    uint32_t height,
    const std::vector<uint8_t>& rgb) {
    const auto png = EncodePngFromRgb8(width, height, rgb);
    if (path.empty() || png.empty()) {
        return;
    }
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("could not write PNG screenshot: " + path.string());
    }
    file.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
}

void WriteBmpFromRgb8Framebuffer(
    const std::filesystem::path& path,
    uint32_t width,
    uint32_t height,
    const std::vector<uint8_t>& rgb) {
    if (path.empty() || width == 0 || height == 0 || rgb.size() < static_cast<size_t>(width) * height * 3u) {
        return;
    }

    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    const uint32_t rowStride = ((width * 3u) + 3u) & ~3u;
    const uint32_t pixelDataSize = rowStride * height;
    const uint32_t fileHeaderSize = 14;
    const uint32_t dibHeaderSize = 40;
    const uint32_t pixelOffset = fileHeaderSize + dibHeaderSize;
    const uint32_t fileSize = pixelOffset + pixelDataSize;

    std::ofstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("could not write BMP screenshot: " + path.string());
    }

    file.put('B');
    file.put('M');
    WriteLe32(file, fileSize);
    WriteLe16(file, 0);
    WriteLe16(file, 0);
    WriteLe32(file, pixelOffset);
    WriteLe32(file, dibHeaderSize);
    WriteLe32(file, width);
    WriteLe32(file, height);
    WriteLe16(file, 1);
    WriteLe16(file, 24);
    WriteLe32(file, 0);
    WriteLe32(file, pixelDataSize);
    WriteLe32(file, 2835);
    WriteLe32(file, 2835);
    WriteLe32(file, 0);
    WriteLe32(file, 0);

    std::vector<uint8_t> row(rowStride, 0);
    for (uint32_t y = 0; y < height; ++y) {
        const uint32_t srcY = height - 1u - y;
        std::fill(row.begin(), row.end(), 0);
        for (uint32_t x = 0; x < width; ++x) {
            const size_t source = (static_cast<size_t>(srcY) * width + x) * 3u;
            const uint8_t r = rgb[source + 0u];
            const uint8_t g = rgb[source + 1u];
            const uint8_t b = rgb[source + 2u];
            row[x * 3u + 0u] = b;
            row[x * 3u + 1u] = g;
            row[x * 3u + 2u] = r;
        }
        file.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
    }
}

void WriteBmpFromRgba5551Framebuffer(
    const std::filesystem::path& path,
    uint32_t width,
    uint32_t height,
    const std::vector<uint16_t>& rgba16) {
    if (rgba16.size() < static_cast<size_t>(width) * height) {
        return;
    }
    std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3u);
    for (size_t index = 0; index < static_cast<size_t>(width) * height; ++index) {
        rgb[index * 3u + 0u] = ExpandRgba5551Channel(rgba16[index] >> 11);
        rgb[index * 3u + 1u] = ExpandRgba5551Channel(rgba16[index] >> 6);
        rgb[index * 3u + 2u] = ExpandRgba5551Channel(rgba16[index] >> 1);
    }
    WriteBmpFromRgb8Framebuffer(path, width, height, rgb);
}

std::filesystem::path ScreenshotSequenceFramePath(const std::filesystem::path& path, uint32_t frameIndex) {
    std::ostringstream name;
    name << path.stem().string() << "_" << std::setw(6) << std::setfill('0') << frameIndex;
    const std::string extension = path.extension().empty() ? ".bmp" : path.extension().string();
    return path.parent_path() / (name.str() + extension);
}
