# ctr-recomp: descoberta de funções no lugar do inventário manual (OoT3D)

## Pergunta

O whole AOT do OoT3D sai do inventário revisado à mão
(`upstream/analysis/codebin_function_inventory.csv` + `runtime_discovered_entries.csv`,
via `generate_whole_aot.py`). Os outros títulos já usam a descoberta automática
(`discover_functions.py`, chamada pelo `tools/ctr/ctr_convert.py`). A descoberta pode
substituir o CSV no OoT3D, que é o único título com inventário revisado para servir de
gabarito? O boot do OoT3D com o AOT descoberto se comporta igual?

## Fontes

- Entradas: `~/oot3d-work/extract/exefs/code.bin` (EUR canônico, md5 8b180959…) e
  `~/oot3d-work/extract/ncch0/exheader.bin` (link para o ExHeader USA, title id
  0004000000033500). São os caminhos operacionais do build do Switch.
- Descoberta: `discover_functions.py --code … --exheader … --compare codebin_function_inventory.csv
  --program-output aot_program.json --selection-output functions.json`, com o Python
  `~/.venvs/triaevum` (capstone 5.0.7), 9 min 53 s no notebook.
- Revisado com o gerador atual: `OOT3D_OPERATIONAL_oot3dExtractRoot=~/oot3d-work/extract
  generate_whole_aot.py --force`. Os mesmos números do `~/oot3d-work/gen/aot_program.json`
  de 16/10.
- C++: `generate_whole_aot_cpp.py --shards 256`, com seleção `whole_aot_functions.json`
  (revisado) e seleção descoberta.
- Boot probe: `tools/ctr/build_title_boot_probe.sh --run --vblanks 300` (perfil `auto`,
  que dá `oot3d`), compilado e rodado no WSL do Ryzen (-O0, 12 jobs). Relatórios em
  `~/oot3d-work/conv/oot3d-discover/probe/report-{revisado,descoberto,nativos}.json`.

## Conclusão

| | revisado | descoberto |
|---|---|---|
| funções | 12442 | 14582 |
| blocos | 162766 | 162942 |
| instruções do guest | 924535 | 924535 |
| blocos sem dono / arestas sem destino | 0 / 0 | 0 / 0 |
| instruções no C++ gerado | 963498 | 1135722 (+17,9%) |
| tamanho do C++ | 344 MB | 405 MB |

- As duas cobrem exatamente as mesmas instruções. Muda só o corte em funções.
- **70 entradas do inventário não viram função na descoberta.**
  - 54 são começo de bloco dentro de outra função.
  - 16 caem no meio de um bloco.
  - Nenhuma é usada pelo runtime, exceto 0x3025C0 (`sys_commonIntrHandler`). Ela é um gancho de diagnóstico por bloco e continua sendo começo de bloco na descoberta.
- **2210 entradas existem só na descoberta.**
  - 2187 são `address_taken` e 23 são `data_pointer`.
  - 1971 já eram começo de bloco no revisado; 239 partem um bloco.
- **Boot probe, 300 vblanks:** revisado e descoberto ficaram idênticos.
  - 2238 SVCs iguais um a um.
  - Os 512 snapshots de memória e as threads iguais.
  - `run`, `boundary`, `frame_boundary` e `state` iguais.
  - Única diferença: `manual_compiled_calls` deu 447 no revisado e **0 no descoberto**.
- **Causa das 447 chamadas que sumiram.** A seleção revisada deixa externas 3 funções que o OoT3D roda como nativo do host: 0x307BD8 `PicaCommandWriter_WriteRegisterRange`, 0x36C174 `Mtx3x4_Multiply` e 0x372224 `Mtx3x4_CopyIfDistinct`. A seleção descoberta traduzia as três, então o comportamento ficava igual, mas sem os nativos.
  - Correção: `closed_selection(manifest, keep_external)`, e `--external-from whole_aot_functions.json` na linha de comando.
  - Com isso o probe descoberto ficou igual ao revisado em tudo, inclusive nas 447 chamadas manuais. Só `entry_points` muda (162757 contra 162933).

## Decisão

- **O caminho descoberto agora existe para o OoT3D.**
  - Rode `discover_functions.py … --program-output P --selection-output S --external-from tools/oot3d/native_a32_runtime/whole_aot_functions.json`.
  - Configure o build com `-DOOT3D_WHOLE_AOT_PROGRAM=P -DOOT3D_WHOLE_AOT_SELECTION=S`, como o `build-switch` faz com o programa revisado.
- **O padrão do OoT3D continua sendo o inventário revisado.** O boot igual só cobre o começo do processo: 0 fronteiras de quadro em 300 vblanks, parado em `ArbitrateAddress` no pc 0x30B394 nos dois. O C++ descoberto tem 17,9% mais instruções e 2140 funções a mais, e o frame time no Switch não foi medido. Trocar o padrão é decisão do Gustavo depois de um tour no Switch.
- **Títulos genéricos** continuam com a descoberta pura (`ctr_convert.py`). Eles não têm nativos do título.

## Alternativas rejeitadas

- **Trocar o padrão do OoT3D já.** Muda o C++ do port do Switch sem medida de frame time.
- **Somar as entradas do inventário às raízes da descoberta.** Isso não deixaria o C++ igual, porque continuariam as 2210 funções extras, e o título genérico não tem inventário.
- **Comparar só o programa (JSON).** Isso não pegaria os nativos perdidos. Quem mostrou foi o probe (`manual_compiled_calls`).

## Em aberto

- **Jogo de verdade com o AOT descoberto.** A janela até o gameplay e um tour no Switch medindo frame time ainda não foram feitos. O probe do OoT3D não passa do começo do boot.
- **Probe com `--complete-gpu-work`.** Revisado e descoberto param iguais no vblank 14, com fault kind=4 no pc 0x0045624C. É limite do probe no OoT3D, não da descoberta, e não foi investigado.
- **`build_title_boot_probe.sh` morre sem `lm-sensors`.** O `temp()` falha com `pipefail` e o ninja fica órfão compilando. No Ryzen contornei com um `sensors` vazio no PATH; o script não foi corrigido.
- **ExHeader e code.bin de regiões diferentes.** O `extract/ncch0/exheader.bin` é o USA, com o code.bin EUR canônico. Os dois caminhos usam o mesmo par, então a comparação vale. Não conferi se o ExHeader EUR muda algo.
