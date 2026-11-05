// Texture pack hash (Azahar "new hash"): CityHash64 of the base level's native bytes.
// Usage: texture_hash file [offset size]... -> prints the hash in uppercase hexadecimal, 16 digits.
#include "fast/renderer/content_hash.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s file [offset size]\n", argv[0]);
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t offset = argc > 2 ? std::strtoull(argv[2], nullptr, 0) : 0U;
    size_t size = argc > 3 ? std::strtoull(argv[3], nullptr, 0) : data.size() - offset;
    if (offset + size > data.size()) {
        std::fprintf(stderr, "out of file bounds\n");
        return 1;
    }
    const auto hash = Fast::Renderer::ContentHash64(std::span<const uint8_t>(data).subspan(offset, size));
    std::printf("%016llX\n", static_cast<unsigned long long>(hash));
}
