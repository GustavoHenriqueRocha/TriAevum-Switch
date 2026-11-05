# Gera um .cpp com os RomFS substitutos como arrays (cmake -P).
#   -DOUTPUT=<arquivo .cpp> -DDATA_DIR=<pasta data/> -DNAMES=a;b;c
# Cada data/<nome>.romfs vira kCtrOpenSourceArchive_<nome>[] e
# kCtrOpenSourceArchive_<nome>_size, no namespace Oot3dNativeGame.
if(NOT OUTPUT OR NOT DATA_DIR OR NOT NAMES)
    message(FATAL_ERROR "embed_romfs.cmake: OUTPUT, DATA_DIR e NAMES são obrigatórios")
endif()
string(REPLACE "," ";" _names "${NAMES}")
set(_text "// Gerado por embed_romfs.cmake a partir de tools/oot3d/third_party/ctr_open_source_archives/data.\n")
string(APPEND _text "// Dados: GPL-2.0-or-later / OFL-1.1; ver o README.md daquela pasta. Não editar.\n")
string(APPEND _text "#include <cstddef>\n\nnamespace Oot3dNativeGame {\n")
foreach(_name IN LISTS _names)
    set(_input "${DATA_DIR}/${_name}.romfs")
    file(SIZE "${_input}" _size)
    file(READ "${_input}" _hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}")
    # Uma linha a cada 32 bytes, para o compilador e para diffs de depuração.
    string(REGEX REPLACE "((0x..,){32})" "\\1\n" _bytes "${_bytes}")
    string(APPEND _text "extern const unsigned char kCtrOpenSourceArchive_${_name}[];\n")
    string(APPEND _text "extern const std::size_t kCtrOpenSourceArchive_${_name}_size;\n")
    string(APPEND _text "alignas(16) const unsigned char kCtrOpenSourceArchive_${_name}[] = {\n${_bytes}\n};\n")
    string(APPEND _text "const std::size_t kCtrOpenSourceArchive_${_name}_size = ${_size};\n\n")
endforeach()
string(APPEND _text "} // namespace Oot3dNativeGame\n")
file(WRITE "${OUTPUT}.tmp" "${_text}")
file(RENAME "${OUTPUT}.tmp" "${OUTPUT}")
