# MM3D: texturas com texto em PT-BR (mapas, telas de dia, menus)

Data: 2026-11-23 · Título: MM3D (EUR Rev 1) · Commits: ver `git log -- tools/oot3d/ptbr/mm3d_texturas.py`

## Pergunta
As falas PT-BR já entram no slot do espanhol (`eus.gmsg`, ver 2026-11-23-mm3d-ptbr.md). Falta o texto que está em
imagem: 102 arquivos do espanhol na romfs. Que formato tem cada um, como ler/escrever sem estragar nada, e como
desenhar o PT-BR no mesmo formato e tamanho.

## O que já existia (procurado antes)
- OoT3D (`tools/oot3d/ptbr/`): `ctxb.py` (decodifica RGBA8/RGBA4/LA8/RGB565/RGBA5551/ETC1/ETC1A4, codifica
  RGBA4/RGBA8/LA8/ETC1A4 inteiro com etcpak), `rotulos.py`/`trocar.py`/`lote_menus.py` (apaga o rótulo e redesenha
  com M PLUS Rounded 1c), `titulos.py` (títulos de área com Belleza). Reaproveitados: decodificador, `codificar_ctxb`,
  a técnica de desenhar 4x maior e reduzir com LANCZOS, a Belleza.
- Upstream (`origin/main`): `tools/mm3d/restoration_layout.py` (ec3a6ea8) tem `parse_gar`/`emit_gar` canônicos,
  byte a byte; `titles/mm3d/dekui/setup/dekui_gar.py` lê GAR e MFL. Nenhum lê `genbamap` nem `daytelop`.
  `gar.py` aqui segue o mesmo layout (crédito no cabeçalho, GPL-2.0-or-later como o upstream).

## Inventário (comando: `python -I mm3d_texturas.py inventario`)
Todos os 102 arquivos são CTXB (cabeçalho 0x48, uma textura, sem mipmap) ou GAR v2 com CTXB dentro: 114 texturas.

| grupo | arquivos | texturas | formato | tamanho | texto |
|---|---|---|---|---|---|
| mapas `layout/map/genbamap/EU_Spanish/000–084.ctxb` | 85 | 85 | ETC1 (84), ETC1A4 (055) | 256x256 | nome do lugar numa placa embaixo (84); o 000 é um aviso em japonês, sem espanhol |
| telas de dia `menu/daytelop/eu-es/` | 7 | 7 | ETC1 | 256x32 (24/48/72), 256x64 (first/scound/last/new) | "- Quedan N horas -", "Amanecer del primer/segundo día", "…del día final", "…de un nuevo día" |
| menus `layout/EU_Spanish/Joker.*.gar` | 10 | 22 | ETC1A4 (17), RGBA4 (2), RGBA8 (1), L8 (1) | 64x32 a 512x256 | 21 com texto (o `plate.ctxb` não tem) |

Menus, por textura (o que mostra): `the_end_text` (L8 128x32, "Fin"); `menu_file_select_parts00` (RGBA8 512x256,
Comenzar/Opciones/Copiar/Borrar/Máscaras/ordinais); `logo_countdown` ("¡Ya!"); `hud_patrs00_action_text` (512x256,
~35 verbos de ação do botão A + Equipo/Mapa/Máscaras/Inventario/Ver/Sin mapa/Volver); `menu_hint_movie_parts00/01`;
`menu_option_parts00/01` (opções de controle e volume); `logo_perfect_parts00` ("¡PERFECTO!" em dois estilos);
`menu_collect_parts00`; `menu_item_parts00`; `menu_map_parts00` (Elegir, PB, ordinais, S1/S2); `world_map_base00`
(~30 nomes de lugar do mapa-múndi + Alejar/Acercar/Seleccionar); `menu_quest_list_parts00`; `menu_schedule_parts00`
(caderno dos Bombers: dias, Activar/Desactivar/Avanzar/Alarma/Eventos); `menu_startup_parts00` ("PULSA START",
aviso do HOME); `btn_return`, `menu_common_parts00/01`, `menu_popup_parts00` (Sí/No/Borrar/Salir, escolha de arquivo),
`parts` (RGBA4, "Todo el día", "Amanecer del segundo/día final").

## Conclusão
- **Ida e volta byte a byte:** os 10 GAR remontam iguais (`gar.ler` recusa o que não remonta) e as 114 texturas
  passam por `decodificar` → `codificar_blocos` com os mesmos bytes (L8/RGBA4/RGBA8 são sem perda; no ETC1/ETC1A4
  só bloco com pixel diferente é recomprimido, então imagem igual → arquivo igual). Conferido nos dados reais
  (32 s) e em teste sintético (`test_ptbr.py`: `Mm3dGar`, `Mm3dCtxb`, `Mm3dPlaca`, 10 testes vistos falhar antes).
- **Placa dos mapas é a mesma nos 85 mapas e nos 6 idiomas** (pixel a pixel fora do texto, ex. (12,220) igual nos
  12 conferidos). A placa limpa sai dos ~490 mapas ETC1 de todos os idiomas: onde o bloco ETC1 mais comum aparece em
  mais da metade deles, ele é a placa e entra com os bytes do jogo; no meio (sempre tem letra), mediana por pixel dos
  mapas sem tinta perto. Medidas do texto espanhol: altura-x 9 px (linhas 218–226), base em 227, centrado em x 128,
  tinta ≈ (38,20,4); texto mais largo 221 px (059).
- **Resultado (23/11):** 91 texturas PT-BR (84 mapas + 7 telas de dia) gravadas em `romfs-ptbr.bin` pelo
  `mm3d.py montar`; os 91 arquivos lidos de volta da romfs são iguais aos gerados. 19 794 de 349 696 blocos
  ETC1 recomprimidos (o resto é byte a byte o do jogo); ~200 blocos por mapa, só na placa.
- **Peso da letra (pedido do Gustavo, 23/11):** calibrado desenhando o PRÓPRIO texto espanhol com a nossa fonte e
  comparando com o original a massa de tinta e a largura (razão nossa/original; 1,00 = igual). Mapas: Noto Sans CJK
  Black, massa 0,982 e largura 0,999 na média dos 84 (mín./máx. da massa 0,95/1,004); antes, com Bold, a massa era
  0,86. Telas de dia (massa, largura por linha): 24/48/72 h (1,04; 0,994); first (0,984; 1,0) e (1,025; 1,016);
  second (1,016; 1,015) e (1,025; 0,994); last (1,019; 0,996) e (1,044; 1,007); new (0,984; 1,01) e (1,003; 1,005).
  Parâmetros em `DIAS`/`PESO_MAPA` de `mm3d_texturas.py`; comparação ampliada (original | espanhol nosso | PT) em
  `texturas/comparacao_peso.png`.
- **Menus (23/11):** 20 texturas (19 do `lote_menus_mm3d.LOTE`, 115 rótulos, mais o mapa-múndi com 35 nomes),
  pelo método do `lote_menus.py` do OoT3D: caixa de cada rótulo + PT. O fundo sai do que é igual nos 6 idiomas
  (`regioes.mascara_dif`); em botão (o fundo muda por idioma) sai da cor mais comum de luma média de cada linha, o que
  mantém o degradê. Cor da letra/contorno, altura-x e espessura do traço são medidas no espanhol; o peso da Noto Sans
  CJK (Medium/Bold/Black) é o de traço mais parecido. Onde a medida erra (logos com brilho, botões pequenos com
  ícone, "¡Nuevo!" de contorno duplo) a entrada do lote leva `xh`/`base`/`cores`/`camadas`/`fundo` medidos à mão no
  espanhol (mapa ASCII da textura). Gravados pelo `mm3d.py montar`: 101 arquivos, 111 texturas (91 + 20) lidas de
  volta da romfs iguais às geradas. Amostras (ES à esquerda, PT à direita) em `texturas/amostra_menu_*.png`.
- **Correções depois da 1ª revisão (23/11):** o fundo transparente de "Visões" virava um retângulo cinza porque o
  anel em volta tinha frisos e a letra de "Canção do Tempo Duplo" (opacos); agora vale também o fundo dos lados da
  caixa nas linhas do texto, fora o que muda por idioma (teste `test_apagar_transparente_com_rotulo_vizinho`, visto
  falhar antes). "Itens" do HUD ganhou fundo azul liso e o tamanho/peso de "Máscaras"; "Canção do Tempo Duplo" em
  Medium sem esmagar; HUD "Olhar" → "Mirar" (glossário: L-Targeting = "Apontar com Mira").
- Tabela es→pt dos mapas sai das próprias falas: o nome espanhol da placa é igual ao de uma fala de nome de área
  (ids 0x100–0x1a0) e o PT é o dessa fala no `eus-ptbr.gmsg`, então placa e falas dizem a mesma coisa. 26 de 28
  nomes saíram assim; à mão: "Ciudad Reloj" → "Cidade do Relógio" (glossário), "Camino al pantano del sur" →
  "Rumo ao Pântano do Sul" (ver Em aberto).

## Decisão
- Fonte das letras: **Noto Sans CJK** (OFL; Black nas placas e nas telas de dia, peso escolhido pelo traço nos menus,
  Regular nas opções e no "APERTE START", que são finos no original) e **Belleza** (OFL, já
  usada no OoT3D) no "- Restam N horas -". Escolhida comparando 10 fontes livres lado a lado com o original
  (`texturas/fontes_cand.png`): o latim da Noto Sans CJK é o mais parecido com o das placas e dos amanheceres.
- Texto PT das telas de dia segue as falas e o glossário: "- Restam 24 horas -" (falas 0x1bb2: "Restam  horas"),
  "Amanhecer do|Primeiro Dia", "Amanhecer do|Segundo Dia", "Amanhecer do|Último Dia" (glossário First/Second/Final
  Day; falas 0x1b98 "Amanhecer do Primeiro Dia"), "Amanhecer de|um Novo Dia". No `daytelop_last` a altura-x caiu de
  20 para 18: o acento do "Ú" encostava na linha de cima.
- Dados do jogo fora do repositório: transcrição do espanhol (`mapas-es.json`), tabela (`tabela-es-pt.json`), placa
  limpa, PNG e CTXB ficam em `~/oot3d-work/conv/mm3d-ptbr/texturas/`. No repo só código e teste sintético.

- Texto PT dos menus: das falas/glossário quando existe (nomes do mapa-múndi pela tabela dos mapas; Equipo →
  "Inventário", Inventario → "Itens", Alarma → "Alarme", Pista → "Dica", Taya → "Tatl"); verbos do HUD pelo
  glossário do OoT3D. Ficam sem troca: os ordinais (1.°/2.°, iguais em PT), o `menu_common_parts01` (só ordinais),
  o aviso "Pulsa HOME…" da tela de início e os rótulos iguais em PT (Mapa, Máscaras, Final, Normal, Alto, Copiar…).

## Alternativas rejeitadas
- **Detectar a caixa de cada rótulo sozinho** (diferença entre idiomas + cortes XY): juntava linhas da grade do HUD e
  pegava os ícones dos botões; ficou a caixa explícita por rótulo, como no OoT3D.
- **Mesma altura-x para todos os rótulos da textura:** os menus misturam três ou quatro tamanhos; a mediana é por grupo
  (`grade`) e o resto mede o próprio rótulo.
- **Recortar letras das texturas dos 6 idiomas** (fonte do próprio jogo): as letras estão em ETC1 (artefato de bloco
  que dobraria ao recomprimir), cada tela de dia tem estilo próprio e faltam letras ("h" e "P"/"D" maiúsculos não
  existem nos amanheceres em espanhol). A fonte do jogo `ltn16.gzf` é a das falas, outro desenho.
- **Recomprimir a textura inteira com etcpak** (como `codificar_ctxb` faz no OoT3D): mexeria nos 4096 blocos do mapa
  e perderia qualidade no desenho todo; recomprimir só o que mudou mantém o mapa idêntico.
- **M PLUS Rounded 1c** (a dos menus do OoT3D): ponta arredondada, diferente das placas do MM3D.
- **Bloco mais comum também no meio da placa**: no meio quase todo nome tem letra, o bloco mais comum seria um pedaço
  de "Este de Ciudad Reloj".

## Em aberto
- **Menus para o Gustavo decidir:** termos novos sem fala correspondente ("Para onde voar?", "Dica em vídeo/foto",
  "Sem fotos", "T" no lugar de "PB" do mapa, "Já!", "Feito!", "APERTE START", "Canção do Tempo Duplo", "Info" para
  Detalles, "Esmagar" para Machacar); "PERFEITO!" e "Já!" em cor lisa (o original tem degradê/brilho); ordinais e o
  aviso HOME ficaram em espanhol.
- **Conferir no jogo:** nada foi visto no console ainda. As telas de dia são máscaras (ETC1 sem alfa, branco sobre
  preto; o `daytelop_new` é preto sobre branco com borda amarelada): repeti o esquema, mas o efeito final só no jogo.
- **"Road to Southern Swamp":** a fala 0x14c está "Ao Pântano do Sul", as irmãs 0x14d–0x150 "Rumo à/a …". A placa usa
  "Rumo ao Pântano do Sul"; a fala não foi mexida (decisão do Gustavo).
- **"Amanhecer de um Novo Dia"** (maiúsculas) e o 000 em japonês (ficou como está) esperam o Gustavo.
- Os mapas `genbamap` de alguns idiomas têm arquivo de 0 byte na romfs (5 do holandês: 006, 065, 067, 068, 074);
  não afeta o espanhol.
