# TriAevum: como investigar e onde procurar

Respostas e comentários em português do Brasil.

## Antes de investigar: procurar o que já foi feito

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
