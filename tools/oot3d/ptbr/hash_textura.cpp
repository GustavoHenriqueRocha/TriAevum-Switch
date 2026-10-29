// Hash do pack de texturas (Azahar "new hash"): CityHash64 dos bytes nativos do nível base.
// Uso: hash_textura arquivo [offset tamanho]... -> imprime o hash em hexadecimal maiúsculo, 16 dígitos.
#include "fast/renderer/content_hash.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "uso: %s arquivo [offset tamanho]\n", argv[0]);
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<uint8_t> dados((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t offset = argc > 2 ? std::strtoull(argv[2], nullptr, 0) : 0U;
    size_t tamanho = argc > 3 ? std::strtoull(argv[3], nullptr, 0) : dados.size() - offset;
    if (offset + tamanho > dados.size()) {
        std::fprintf(stderr, "fora do arquivo\n");
        return 1;
    }
    const auto hash = Fast::Renderer::ContentHash64(std::span<const uint8_t>(dados).subspan(offset, tamanho));
    std::printf("%016llX\n", static_cast<unsigned long long>(hash));
}
