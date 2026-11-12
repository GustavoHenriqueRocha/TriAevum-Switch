# Mescla do fork do John (JohnUzoka/TriAevum-Switch, branch english) no deko3d

Data: 2026-11-12 · Título(s): OoT3D (e o genérico do ctr-recomp, que não pode mudar) · Branch: `integra-john-2`

## Pergunta
O Gustavo pediu "pode trazer as melhorias" do John, "eu quero a grama também". Dá para trazer o
`english` inteiro (301 commits à frente da base comum 311d8fd7) sem perder o que só o nosso fork tem
(ctr-recomp: SM3DL/MK7, PiP da tela de baixo, salto de command buffer da PICA, uber leve, perfil de
título) nem mudar o OoT3D por acidente?

## Fontes
- `git merge --no-ff --no-commit john/english` sobre deko3d 80ca0c24; 68 conflitos no início
  (17 add/add, 51 de conteúdo), resolvidos um a um (diff3).
- `git merge-base deko3d john/english` = 311d8fd7.
- Guardas: `python3 tests/switch/test_bug_guards.py` (600/600 depois da mescla).
- Teste novo: `oot3d_native_pica_frontend_tests` (salto com `list_walk_fast`), visto falhar sem a
  correção ("list_walk_fast ignored a PICA command buffer jump").

## Conclusão
- O `english` traz a grama no deko3d (`config/grass` = `draw`, ou L3+R3 > Vídeo > Grama), o
  atualizador OTA (manual, aponta para o repo do John), mods de RomFS, texturas do HUD baixadas no
  console, segundo núcleo para o som, `list_walk_fast`, `capture_delta`, iluminação por pixel e
  muitas correções de HUD/menus. A maior parte dos nossos commits já estava lá (o John porta os PRs
  dele e os nossos).
- Pontos em que os dois lados mudaram a mesma coisa e a mescla teve de juntar os dois:
  - **Salto de command buffer da PICA (SM3DL)**: o caminho rápido do John (`list_walk_fast`, ligado
    por padrão no jogo) não via o CMDBUF_JUMP0/1; a mescla pura deixaria o SM3DL travado de novo.
    Agora os registradores de salto são um tipo próprio na tabela (`kRegisterJump`) e os dois laços
    usam o mesmo `followPendingJump`.
  - **Gerador de shader paramétrico**: os dois acrescentaram um parâmetro (`lightingLuts` dele,
    `procTex` nosso). Ficam os dois: 6 bases dele × proctex = 12 bases no dump.
  - **Uber leve**: ele tirou as cópias para variáveis locais (menos GPR); nós limitamos o laço do
    TEV pelo número de estágios (`control.z`). O limite foi aplicado também ao avaliador uniforme dele.
  - **Quadros de áudio do DSP**: a nossa função única (`DeliverPendingDspAudioFrames`, janela e boot
    probe) com a espera do segundo núcleo dele dentro.
  - **Importação do dump**: a versão dele + o nosso 3a7ed62f (manifesto com `process.title_id`,
    dump de outro jogo recusado pelo program id), que tinha se perdido na escolha dos add/add.
    Importação, HUD e mods só no lançamento padrão (sem argumentos próprios), como no nosso.
    `config/no_rom_import` voltou.
  - **Ganchos de bloco**: a lista agora vem só de `Oot3dNativeBlockEntryHookPcs` (dele); o
    `static_assert` nosso amarra os PCs da fronteira de quadro do OoT3D aos da lista.
- O runtime dele pede `Oot3dWholeAotHookGates()` no C++ gerado: o `gen/cpp-lean` da p7e não tem.
  O C++ foi regerado em `gen/cpp-john` com as mesmas flags do `cpp-lean` (sem `--sparse-block-entries`
  nem `--fold-vfp-literals`), o que também cumpre a decisão de 06/11 (regerar no próximo build).

## Decisão
Mesclar o `english` inteiro, com as junções acima. Guardas nossas sem par no dele foram
mantidas (82); três foram reescritas para a estrutura nova sem afrouxar o que checam; duas saíram
por duplicadas ou obsoletas (ver o commit da mescla).

## Alternativas rejeitadas
- Escolher commits do John um a um (cherry-pick): 301 commits, a grama depende de dezenas deles.
- Pegar a versão dele em todos os conflitos: perderia o salto da PICA, o PiP, o perfil de título e o
  uber leve (o SM3DL e o MK7 voltariam a travar).

## Em aberto
- Tour no Switch contra a p7e (fluidez) com a grama ligada e desligada.
- O pack de shaders (`deko3d.pack`) refeito a partir do v9 + bases novas; o jogo grava o que faltar.
- Portar para o nosso `HandleWaitSynchronization` a otimização de uma alocação do
  `WaitSynchronizationN` do John (não veio porque muda os nomes dos eventos).
