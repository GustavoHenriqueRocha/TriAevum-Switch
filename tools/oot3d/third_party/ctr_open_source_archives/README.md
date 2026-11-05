# Substitutos open source dos system data archives do 3DS

Alguns títulos abrem arquivos de sistema do console (fonte compartilhada,
recursos do Mii, lista de países, lista de palavras proibidas). O TriAevum não
pede dump do console: serve estes substitutos livres, os mesmos que o
Citra/Azahar embutem quando o arquivo original falta.

| Arquivo em `data/` | Title id servido | Conteúdo | Bytes | SHA-256 |
| --- | --- | --- | --- | --- |
| `shared_font.romfs` | `0004009B00014002` (JPN/EUR/USA); também `00014102` CHN, `00014202` KOR, `00014302` TWN | `cbf_std.bcfnt.lz` (BCFNT em LZ11) | 1110016 | `5c1ffba10c60a5ec11a3e566fc3a4c4ff356403ea9c2802e5a502ffa66405111` |
| `mii.romfs` | `0004009B00010202` | `CFL_Res.dat` | 73600 | `3627a36b166b145afa8ed07576b3c47554aa7fbcd6e4a79ea8b735360fa397de` |
| `country_list.romfs` | `0004009B00010402` | listas de países/regiões (`*_LZ.bin`) | 219040 | `4a4f770d617d93bffa1d6437d1caae0626b31b9f01c15d43aa3ed9bbb566b20b` |
| `bad_word_list.romfs` | `000400DB00010302` | NG word list (`0.txt`…`16.txt`, `version.dat`) | 1508 | `12be8852e0cc399d6d2018f1d8b705954edc230f01e7b68a270c5b9bda50eec6` |

Cada arquivo é a RomFS do NCCH só com o nível 3 do IVFC (o que o FS entrega
ao título ao abrir a RomFS do archive `0x2345678A`).

## Origem

- Gerados por [B3n30/citra_system_archives](https://github.com/B3n30/citra_system_archives)
  (`run.sh` de cada pasta). Commits geradores, registrados no cabeçalho de
  cada `.h` do Azahar: fonte `848af476fb2fad4144962dfb2548bd7841f1b72a`, Mii
  `5e6b5df2f58811b75f62a526f6602732e2ad26db`, países
  `bbe60b5477fc8d7f317d9a82a9f2e6a789af931c`, NG word
  `47cdc9394501a61406529e3037763745c9642b23`.
- Copiados byte a byte de `externals/open_source_archives/include/*.app.romfs.h`
  do [Azahar](https://github.com/azahar-emu/azahar) na revisão
  `e9b14fd1d9a115b21d3321b6f967407582d2c08c` (arrays `xxd -i` convertidos de
  volta para binário; o tamanho confere com `*_len`).
- No build, `embed_romfs.cmake` transforma cada `.romfs` em array C++
  (`CMake/oot3d/host_services.cmake`); quem usa é
  `tools/oot3d/native_game_runtime/oot3d_native_a32_ctr_system_archives.cpp`.

## Licenças

- Repositório gerador e os dados que ele cria (modelos/texturas do Mii,
  `country.json`, glifos desenhados em `reserved_unicode_chars`, ferramentas):
  GNU GPL v2 (`LICENSE-GPL-2.0.txt`, cópia do `license.txt` de lá); as
  ferramentas declaram "GPLv2 or any later version", e o Azahar distribui
  estes dados como parte do seu programa GPL-2.0-or-later. O TriAevum os
  usa nos mesmos termos, sob a opção "or later", dentro do produto
  GPL-3.0-or-later (ver `LICENSE_SCOPE.md`).
- Glifos da fonte compartilhada, rasterizados de:
  - **Montserrat** Regular 4.000, Copyright 2011-2016 The Montserrat Project
    Authors, SIL Open Font License 1.1, sem Reserved Font Name
    (`OFL-1.1-Montserrat.txt`). A fonte derivada continua sob a OFL; a OFL
    permite empacotá-la com software de qualquer licença.
  - **GNU Unifont** 10.0.05, Copyright 1998-2017 Roman Czyborra, Paul Hardy,
    Qianqian Fang, Andrew Miller et al., GPL v2 ou posterior com a GNU Font
    Embedding Exception:

    > As a special exception, if you create a document which uses this font,
    > and embed this font or unaltered portions of this font into the
    > document, this font does not by itself cause the resulting document to
    > be covered by the GNU General Public License. This exception does not
    > however invalidate any other reasons why the document might be covered
    > by the GNU General Public License. If you modify this font, you may
    > extend this exception to your version of the font, but you are not
    > obligated to do so. If you do not wish to do so, delete this exception
    > statement from your version.

Nenhum byte destes arquivos vem de um console ou de software da Nintendo.

## Limitações

- A fonte só cobre o conjunto JPN/EUR/USA; CHN/KOR/TWN recebem a mesma fonte
  (o host avisa em `TRIAEVUM_SYSTEM_ARCHIVE`) e ideogramas da região faltam.
- O desenho dos glifos e as partes do Mii são outros: texto e Miis aparecem
  com outra aparência que no console.
