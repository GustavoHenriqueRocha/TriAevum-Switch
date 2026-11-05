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
