# TriAevum: como investigar e onde procurar

Respostas e comentários em português do Brasil.

## Antes de investigar: procurar o que já foi feito

**Regra dura (06/11):** isso vale antes de implementar, otimizar ou medir também, não só de
investigar. O port OoT3D→Switch já resolveu muita coisa que os títulos do ctr-recomp vão
encontrar (ex.: o SM3DL era medido num build Debug -O1 enquanto o port roda em Release
`-O2 -DNDEBUG`). Procure aqui, nos repos locais e na web (Azahar/Citra, 3dbrew) antes de começar.

Este projeto já investigou muita coisa (887+ commits, 200+ documentos). Antes de
começar uma investigação nova, gaste 2 minutos procurando se ela já existe:

| Onde | O que tem | Como procurar |
|---|---|---|
| `git log` | toda correção tem mensagem explicando causa e número medido | `git log --oneline -i --grep=<palavra>`; `git log -S<símbolo> --oneline` (quando o símbolo entrou/saiu) |
| `docs/` | relatórios, estratégias, handoffs, auditorias | `grep -ril <palavra> docs/`; pontos de entrada: `docs/OOT3D_CURRENT.md`, `docs/DEKO3D_PROGRESSO.md`, `docs/GUIA_PORT_3DS_SWITCH.md` (técnicas reutilizáveis para outros jogos) |
| `tests/switch/test_bug_guards.py --list` | cada bug já corrigido, com arquivo e teste | `python3 tests/switch/test_bug_guards.py --list \| grep -i <palavra>` |
| testes de regressão | `tests/switch/`, `tools/**/test_*.py`, `*_tests.cpp` | o nome/docstring do teste descreve o bug original |
| `~/oot3d-work/bench/` | resultados de tours/benchmarks no Switch (frame time por área) | uma pasta por versão (p6z, p7e, ...) |
| `~/oot3d-work/jogos/<título>/` e `~/oot3d-work/conv/<título>/` | extrações, inventários, C++ gerado e relatórios do boot probe de outros títulos | `build/title/report.json` e `stderr.log` |
| memória do Claude (`~/.claude/projects/-home-gustavo/memory/`) | decisões, estado de cada frente, preferências | `grep -ril <palavra>` lá |

Registre o que descobrir no mesmo lugar: mensagem de commit com causa e número,
guarda/teste de regressão, e técnica nova em `docs/GUIA_PORT_3DS_SWITCH.md`.

## Escreva tudo o que não virou código

Pesquisa que fica só na conversa some no fim da sessão. Toda investigação,
pesquisa de referência (Azahar, 3dbrew, outros projetos) ou decisão de design
vira um arquivo em `docs/pesquisas/AAAA-MM-DD-<assunto>.md`, mesmo quando não
gerou código. Modelo em `docs/pesquisas/README.md`. O essencial:

- **Pergunta**: o que se queria saber e por quê.
- **Fontes**: arquivo:linha, commit, link do Azahar/3dbrew, comando que reproduz.
- **Conclusão**: o que é verdade, com número quando houver.
- **Decisão e alternativas rejeitadas**: o que se escolheu, o que foi descartado
  e por que (é o que evita refazer a mesma pesquisa daqui a um mês).
- **Em aberto**: o que não se conseguiu confirmar.

Relatório de agente/workflow também vai para lá, depois de verificado.

## Regras de trabalho

- **Teste de regressão antes da correção.** Escreva o teste, veja ele falhar
  pelo motivo certo, depois corrija. Teste que nunca falhou não prova nada.
- **Alegação não é fato.** Achado de agente, comentário antigo, nome de função
  ou doc desatualizado é hipótese até ser conferido no código, no log ou na
  fonte de verdade. Por isso os workflows têm etapa de verificação adversarial.
- **Nunca enfraquecer teste ou guarda para passar.** Não afrouxar limite de frame
  time, não pular teste, não apagar guarda de `test_bug_guards.py`. Se o esperado
  mudou de verdade, o commit explica por que o antigo estava errado.
- **Teste sem a ROM.** Testes de regressão usam dado sintético montado à mão
  (imagens NCCH/BLZ, programas A32 mínimos, comandos PICA), para rodar em
  qualquer máquina e em CI sem arquivo do jogo.
- **Medida que oscila se repete.** Frame time e tours no Switch têm ruído:
  compare pelo menos duas rodadas antes de chamar de melhora ou regressão.
- **Pronto é no commit exato.** Testes rodados no SHA que foi para o Switch ou
  para o GitHub. Mais de 3 correções numa sessão: revise o intervalo inteiro
  (`git diff <início>..HEAD`) antes do push.
- **Relatório de agente usa três estados:** confirmado, parcial ou sem suporte,
  cada um com a evidência.
- **Teste de jogo na janela entra na pista.** Chegar ao menu não basta: a rodada tem de entrar
  em jogo de verdade (no MK7, uma corrida) e ficar alguns segundos lá, com captura. O MK7 caiu
  ao entrar na corrida (block_32, 10/11) depois de várias rodadas "verdes" que paravam no menu.
  Timeline do MK7: `tools/oot3d/native_game_runtime/input_timelines/mk7_title_to_race.json`
  (A por 40 quadros, solto por 20, até o quadro 7200; com o save já criado chega à pista em uns
  1500 quadros). Prefira pulsos contínuos a quadros fixos: a velocidade da janela varia.
- **Rodada de teste da janela vai sem som:** `--disable-audio` (depois do `--` do
  `build_title_window.sh`). Só ligue o som quando o teste for do áudio. O flag só corta a saída
  para o host; o DSP continua mixando e o guest recebe os quadros, então o jogo não muda.
- **Comportamento do OoT3D não muda por acidente.** Tudo que for genérico passa
  pelo perfil de título (`ctr_title_profile.h`) e ganha guarda em
  `tests/switch/test_bug_guards.py`.

## Método de investigação

1. **Reproduza por comando, 100% das vezes.** Sem repro determinística é caça a
   fantasma. Exemplos que já existem: o boot probe (`tools/ctr/ctr_convert.py ...
   --run`), os tours do lab no Switch (`lab/script.jsonl`), os testes de host.
2. **Siga transições de estado, não nomes.** A pergunta útil é "quais bytes,
   registradores ou chamadas mudam quando o sintoma aparece?", não "qual função
   parece suspeita?". Compare dois relatórios (antes/depois, Zelda/Mario) campo a campo.
3. **Shotgun primeiro para mapear o terreno.** Altere uma categoria inteira de uma
   vez (todas as texturas, todos os shaders, todas as funções de um shard) e veja
   em segundos se ela importa. Só depois afunile para o item.
4. **Meça antes e depois, com número.** "Melhorou" não vale; vale "60,9 → 12,5
   quedas por janela" ou "57 s → 0 s". O critério de fluidez é frame time
   (nenhum quadro > 16,7 ms), não FPS médio.
5. **Não aceite "funciona o bastante".** Se o resultado está certo por motivo
   desconhecido, a investigação não acabou: entenda o porquê ou registre
   explicitamente o que ficou em aberto.
6. **Em loop, troque o ângulo.** Se a mesma hipótese voltou duas vezes, mude de
   abordagem: outro agente/modelo, outra fonte de verdade (Azahar, manual ARM,
   3dbrew), ou um experimento shotgun.
7. **Fontes de verdade externas:** Azahar/Citra (HLE de serviços, PICA), ARM ARM
   (semântica de instrução), 3dbrew (formatos, IPC, SVCs). Confira contra elas
   antes de inventar comportamento.

## Armadilhas conhecidas

Formato: sintoma → causa → o que fazer. Detalhe em `docs/pesquisas/` e nos commits citados.

- **Limpar `.claude/worktrees` em massa leva junto o deko3d** → os worktrees vivos (`deko3d`, `mk7`,
  `ctr-discover`) ficam na mesma pasta que os `agent-*`, e muitos worktrees velhos estavam em HEAD solta
  com commit que não existia em nenhum branch. → Limpe só `agent-*`; antes, `git branch -a --contains`
  em cada HEAD solta e crie `resgate/<nome>` para o que não estiver em branch. Em 12/11 saíram os 51
  e foram recriados `deko3d`/`mk7`/`ctr-discover` e 14 `resgate/*` (backup do que estava fora do git em
  ~/Documentos/backup-triaevum-worktrees-2026-11-12.tar.gz).

- **Crash com "Function not implemented" (p6x)** → `std::thread::detach()` lança
  `std::system_error`: a newlib do devkitA64 não tem `pthread_detach` (ENOSYS). → Nunca `detach()`
  no Switch; guarde a thread num objeto que nunca é destruído (p6y).
- **Exceções C++ abortam no Switch** → `-ffast-math` na ligação some com o terminador do
  `.eh_frame`. → Não use `-ffast-math` na ligação.
- **Corrupção de memória do deko3d (p6d–p6j)** → `dkCmdBufBindVtxBuffers` do deko3d 0.5.0 reserva
  5 palavras por buffer e grava 6. → Mantenha `kCommandGuardBytes` (4 KiB de folga por pedaço).
- **Build "rápido" virou 740 etapas (~30 min, 16/10)** → apagar `.ninja_log`/`.ninja_deps` (ou o
  `build.ninja`) na cópia da pasta de build. → Preserve os três; antes de prometer, conte as etapas
  com `ninja -n | wc -l` e diga o tempo real.
- **"Erro que se repete" no `perf.txt`/`hang.txt`** → os dois ACUMULAM sessões; a "corrupção
  determinística" de 26/10 era uma linha só (sessão 96, tour16) relida a cada tour. → Confira de
  que sessão é a linha (`tools/oot3d/perf_report.py perf.txt --session -1`).
  `perf.txt` parar sem crash report também é saída normal (o jogador fechou o jogo).
- **Comparação de desempenho enganosa** → rodadas com clocks diferentes (tour9 em 1020/307/1331 deu
  40 fps contra rodadas a 1785/768/1600). → Confira a linha `clocks:` do `perf_report` antes de
  comparar. No clock padrão (1020/307) nem o 30 nativo segura: decisão de 26/10, não investir nisso.
  O Switch Frame Tap rouba desempenho: desligue para medir frame time.
- **Queda a 11 fps em Din's Fire/áreas MQ** → shader fora do `deko3d.pack` cai no programa uber
  (GPU 40–119 ms/quadro). → O jogo grava os faltantes em `logs/deko3d_shaders/`; refaça o pack com
  `tools/oot3d/deko3d_shader_pack.py` e guarde o anterior como `deko3d.vN.pack`.
- **HUD com ícones soltos depois do PT-BR (30/10)** → `config/atlas_overrides.o3tu` (TopScreen 3.0)
  não casava com o `menu_top_parts00` redesenhado. → Romfs PT-BR mudou: refaça o atlas pack
  (`--extra-romfs-image`, commits d9359b8f/789e9a62, teste `test_topscreen_30_pack.py`).
- **FTP do Sphaira some** → ele fecha com o jogo aberto. → Comandos durante o jogo vão por
  `lab/script.jsonl` (lido na abertura) com a op `gameplay`; FTP só com o jogo fechado.
- **Wi-Fi caiu durante envio (25/10)** → tráfego acima de 5 MB/s no total. → Um envio por vez:
  `curl --limit-rate 5M`, `scp -l 40000`, `rsync --bwlimit=5000`.
- **Notebook a 96–99 °C** → builds com 3 jobs ou mais de um agente compilando; o `build-local.sh`
  (SIGSTOP a 85 °C, retoma a 75 °C) não segura picos curtos. → No notebook só
  `~/oot3d-work/build-local.sh` com 2 jobs e `LIMITE=80`, um build por vez. Ver
  `docs/pesquisas/2026-10-17-temperatura-builds.md`.
- **Ryzen travou (26/10)** → RAM do Windows esgotada por WSL + build + PowerShells órfãos (teto de
  3 GB/-j 1 na época). → Desde 05/11 à noite o Gustavo liberou o Ryzen a 100% com RAM em 2400 sem
  DOCP: WSL com 12 GB, `JOBS=12 SHARDS=1 bash /root/agentes/deko3d/build.sh build` (fonte em
  `src/`, build em `build/`, montados por `mount --bind` nos caminhos do notebook). Transferência
  até 10 MB/s nos dois sentidos (ida e volta; o Wi-Fi ainda falha, regra de 10/11):
  `rsync -a --bwlimit=9500 --rsync-path='wsl rsync'`, `scp -l 76000`, preservando
  `.ninja_log`/`.ninja_deps`. RAM já teve bit flips: confira md5 depois do relink. O WSL desliga
  quando a última sessão `wsl.exe` fecha: mantenha o `ssh ryzen wsl ...` aberto durante o build.
- **MTP não sobrescreve** → `gio copy` falha em arquivo existente. → `gio remove` antes, ou use o
  FTP (`curl -T` sobrescreve).

## Console/Switch

- Switch **Lite** (Mariko), Atmosphère 1.11.1, FW 22.1.0, sys-clk no clock oficial (CPU 1785);
  overclock acima do oficial ficou fora (decisão de 17/10; guia em
  `~/oot3d-work/GUIA_OVERCLOCK_LITE.md`). O relógio do console está atrasado (prints caem em
  `album_sd:/2026/...`).
- Homebrew pelo **Sphaira** (não DBI). FTP do Sphaira na porta **5000**, pasta
  `/switch/oot3dre/`. O IP muda com a rede: descubra varrendo a porta 5000, nunca fixe.
- O FTP fica sempre aberto fora do jogo: não peça aviso ao Gustavo; espere com loop (com timeout)
  e baixe logs/prints sozinho.
- NRO pronto vai para o Switch sem esperar pedido, rotulado (`~/oot3d-work/enviar-switch.sh` chama
  `tools/oot3d/rotular_nro.py`), com o tamanho conferido depois; avise quando enviou. Ao mandar
  versão nova, apague as NROs do port que não usamos mais em `/switch/oot3dre/` (só elas, sem
  perguntar) e diga o que saiu.
- No SD (`sdmc:/switch/oot3dre/`): `config/<nome>` (arquivo presente liga a opção), `logs/`
  (`perf.txt`, `hang.txt`, `deko3d.txt`, `deko3d_shaders/`), `shadercache/` (`deko3d.pack`),
  `textures/`, `savedata/`, `lab/`, `boot-status.txt`. Crash reports em `atmosphere/crash_reports`.
- Teste no console usa o save NORMAL dos tours, nunca o save Master Quest do Gustavo; antes do 1º
  boot de build nova, backup dos saves (`scripts/switch/backup-saves.sh`, branch `saves-backup`).
- Master Quest sem zerar: flag no `system.dat` (byte 0 = 0xBE, CRC-16/ARC em +0x20), opção
  `config/unlock_master_quest`, sem gravar no save.
- ROM de entrada: USA (No-Intro 0033), convertida para EUR canônico pelo adaptador
  `oot3d-usa-input-canonical-eur-ef210566`. Trabalho fora do git em `~/oot3d-work`.
- **Este fork (TriAevum-Switch) é público**: nunca suba ROM, C++ gerado, `.nro`, `game/` nem
  derivados. Pacote pessoal (NRO + caches + configs) só no repo privado `oot3dre-pessoal`, como release.
- Fluidez acima de FPS médio: nenhum quadro > 16,7 ms em qualquer área de jogo; queda em
  carregamento é aceitável (ignore `ctx=load`). Ao comparar tours, olhe primeiro picos/refresh
  perdidos, hold × interp e imagens distintas por segundo; a média vem depois.
- Bug novo ganha guarda em `tests/switch/test_bug_guards.py` ou teste registrado em
  `tests/switch/run.sh`; experimento rejeitado vira guarda `[decisão]` (não refazer).
