# Temperatura do notebook e onde compilar

Data: 2026-10-17 (revisto até 05/11) · Fonte: memória antiga (`notebook-temperatura.md`,
`ryzen-limite-ram.md`, `triaevum-deko3d.md`), compilada na fase 2 do ai-memory.

## Pergunta
Onde rodar build pesado do port sem fritar o notebook nem travar a Ryzen.

## Fontes
- `sensors` (Package id 0) no notebook; `~/oot3d-work/build-local.sh [jobs]` (SIGSTOP a 85 °C,
  retoma a 75 °C, variável `LIMITE`).
- Ryzen: `/root/agentes/deko3d/guard_build.sh` (systemd-run `MemoryMax=3G`, `ninja -j 1`).

## Conclusão
| Data | O que aconteceu |
|---|---|
| 17/10 | agentes compilando no notebook: 98 °C (limite 100) |
| 23/10 | `build-local.sh -j3`, 256 shards em 88 min: 99 °C apesar do teto (69 pausas) |
| 23/10 | `build-local.sh 2`, 23 etapas: 93 °C, 7 pausas |
| 24/10 | 2–3 agentes compilando: 96–97 °C mesmo com -j1/-j2 |
| 26/10 | Ryzen travou: Windows com 7,5 GB, WSL até 5 GB, Firefox ~1,5 GB, PowerShells órfãos |
| 04/11 | notebook com 3 jobs: 96 °C |
| 05/11 | notebook, 35 objetos: 91 °C (17 pausas) |

O SIGSTOP por amostragem não segura picos curtos. Shards da Ryzen bateram 256/256 com os do notebook.

## Decisão
- Notebook: só pelo `build-local.sh`, 2 jobs, `LIMITE=80`, um build por vez.
- Ryzen: teto de 3 GB e -j1, uma conexão SSH por vez, nada em paralelo com o build. Desde 03/11 o
  WSL está parado (SVM desligada), então o build pesado volta ao notebook.

## Alternativas rejeitadas
- -j3 ou mais no notebook; vários agentes compilando juntos.
- Ryzen sem teto de memória (travou o PC em uso).

## Em aberto
- Quando o WSL da Ryzen voltar, rever a divisão notebook × Ryzen.
