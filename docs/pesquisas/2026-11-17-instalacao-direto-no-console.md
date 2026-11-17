# Instalação direto no console e atualização pelo nosso repo (17/11)

## Pergunta
O Gustavo quis "igual ao do John": instalar e atualizar direto no Switch, com a
tradução PT-BR, o nome dele e update só estável e testado, vindo do nosso projeto.

## O que foi feito e medido
- **Importação no console** (código de 724de6e8): dump USA de 536870912 B em
  `switch/oot3dre/`, import em 34,7 s (`logs/rom_import.txt`). code.bin e exheader.bin
  iguais aos do PC; romfs.bin com o hash da receita (`dae62037…`), igual a
  `~/oot3d-work/usa-adapted/romfs.bin`. O manifesto só ganhou `title_id` (código novo).
  A primeira comparação deu "diferente" porque o download de volta veio cortado
  (137 de 473 MB): baixar do Switch pelo FTP do Sphaira anda a ~0,7 MB/s; o envio é rápido.
- **Lentidão relatada no mesmo teste**: sys-clk desligado (1020/307/1331), 28,8 fps com
  26,8 ms de CPU do jogo por quadro; 0 draws no uber e 0 shaders faltando no `deko3d.txt`.
  O sys-clk ganhou `boot2.flag`; com ele a 1.0.0 rodou a 60 fps (~6 ms do jogo).
  O travamento total do console (HOME parado) não deixou crash report nem `hang.txt`: em aberto.
- **Atualizador** (69d4297c): repo padrão `GustavoHenriqueRocha/oot3dre-pessoal` (privado),
  `config/update_token` só-leitura (fine-grained, Contents: Read-only; conferido: lê 200,
  escrita 403, outro repo privado 404). Sem token o repo privado nunca é consultado.
  No console: `update.txt` "build 1: No release on the stable channel yet." (v1.0.0 é pré-release).
- **Crédito**: autor da NACP e título da tela de importação.
- **PT-BR como mod**: a tradução muda 103 arquivos (20,6 MB) do romfs, nenhum novo; viram
  `mods/ptbr/romfs/`. No console: `romfs_mods=active:90`, 13 "overridden" (07_US_SPANISH
  cai no mesmo alvo de 06_EU_SPANISH; os 13 pares são idênticos), 0 ignorados.
- **Empacotador** `tools/oot3d/switch_release.py` (ca16bbd7): release.json + assets + zip
  (74 MB). Release v1.0.0 build 1 publicada como pré-release em oot3dre-pessoal.

## Decisões e alternativas rejeitadas
- Token no SD (escolha do Gustavo, opção A). Rejeitadas: releases públicas como o John
  (a NRO não pode ser publicada) e servir pelo Ryzen (só funcionaria em casa).
- "Estável e testado" = a mesma release sai como teste e só é promovida a stable
  (`switch_release.py promover`) depois do teste no console; o NRO não é recompilado.
- O mod PT-BR vai só no zip: o atualizador nomeia o asset pelo último componente e os
  menus 06/07 têm nomes iguais.
- Fora do zip: `game/`, saves, `no_rom_import`, `update_token` e o que vem do TopScreen
  (CC BY-NC-ND), que o console gera sozinho.

## Em aberto
- Causa do travamento total do console no teste das 11:49.
- Passo 3 (TopScreen baixado no console) e o levantamento de código morto que o Gustavo pediu.

## Promoção (17/11, tarde)
O Gustavo confirmou no console: textos em PT-BR e aba Updates certa. A v1.0.0 virou
stable (`switch_release.py promover`): pré-release desligado, release.json com
`"channel": "stable"`, lido pelo token só-leitura como o console lê
(1.0.0, build 1, stable). O console com a 1.0.0 vê "mesma build" e não oferece nada.
