# SM3DL parado no applet 0x402 (seletor de Mii)

Data: 2026-11-05 · Título(s): SM3DL (0004000000053F00), genérico · Continuação de
`2026-11-05-sm3dl-logo.md` (item 5 e "Em aberto").

## Pergunta

Depois do logo branco, a thread 0 do SM3DL esperava o applet 0x402 para sempre. O que o APT
precisa responder para o jogo passar do seletor de Mii, sem código por título?

Repro: `tools/ctr/build_title_window.sh --work ~/oot3d-work/conv/sm3dl --run --seconds 180 --
--input-timeline ~/oot3d-work/conv/sm3dl/a_repetido.json` (A por 4 quadros a cada 40), com
`TRIAEVUM_GUEST_STATE_DUMP=120` para ver os nomes dos IPC atendidos.

## Fontes

- Azahar (`git clone --depth 1 --sparse` em `~/oot3d-work/ref/azahar`, commit 7654972):
  - `src/core/hle/service/apt/applet_manager.cpp`: Preload/PrepareToStart/Start/Close de applet
    de biblioteca, GetAppletInfo, IsRegistered, Send/Receive/GlanceParameter, GetLockHandle
    (AutoLibrary → Library), tabela de title IDs por região;
  - `src/core/hle/service/apt/apt.cpp` e `apt_u.cpp`: formato IPC de cada comando;
  - `src/core/hle/applets/mii_selector.cpp` e `frontend/applets/mii_selector.cpp`: Request →
    Response com o bloco de captura; o frontend padrão fecha na hora com o Mii padrão;
  - `src/core/hle/mii.h` e `mii.cpp`: layout de MiiData (0x5C) e CRC (poly 0x1021);
  - `src/core/hle/kernel/shared_memory.cpp`: bloco de applet vem do heap (base ≠ 0), exige
    other_permissions ≠ DontCare; tamanho 0 mapeia nada.
- Logs da janela (`~/oot3d-work/conv/sm3dl/window/stderr*.log`), seis rodadas de 100–180 s.

## Conclusão

Cinco travas em sequência, todas no APT/kernel, nenhuma no jogo:

1. **GetAppletInfo(0x402) em laço (confirmado).** O stub do `PreloadLibraryApplet` respondia
   sucesso sem registrar nada; `GetAppletInfo` voltava zerado (registered = loaded = 0). Antes: 8192+
   chamadas em 180 s, quadro parado.
2. **IsRegistered(0x402) em laço (confirmado).** O SDK chama `PrepareToStartLibraryApplet` depois do
   preload, recebe `AlreadyExists` (o Azahar faz igual) e espera `IsRegistered` virar 1.
3. **SendParameter(Request) recusado (confirmado).** O jogo pede **sem captura**:
   `CaptureBufferInfo` = size 0, formatos 0xFFFFFFFF. Eu tinha tratado size 0 como erro; o Azahar
   cria o bloco vazio e responde.
4. **MapMemoryBlock(bloco vazio, endereço 0) (confirmado).** O host recusava bloco de tamanho 0 e
   exigia DontCare; no Azahar o bloco de applet exige o contrário e tamanho 0 é no-op.
5. **UnmapMemoryBlock não existia no host (confirmado).** Depois do `WakeupByExit` o jogo desmapeia o
   bloco.

Com as cinco correções, a rodada de 180 s mostra o ciclo inteiro: Preload → GetAppletInfo(loaded)
→ IsRegistered(yes) → SendParameter(Request) → Response (Glance + Receive) → StartLibraryApplet →
WakeupByExit (Glance + Receive) → Map/Unmap do bloco vazio.

**Próxima trava (confirmada, não corrigida):** `fs:USER` 0x080B0102 (OpenDirectory) sem HLE na
thread 4 → resultado de comando sem suporte → o jogo chama `ConnectToPort("err:f")` e o processo
para no VBlank ~1080.

## Decisão

- Arquivo novo `oot3d_native_a32_ctr_host_applet.cpp`, chamado no começo do `HandleAptRequest`;
  devolve `nullopt` para o que não é dele (o Wakeup inicial e o resto do APT seguem iguais).
- Slot de applet de biblioteca com só o seletor de Mii (0x401/0x402) em HLE. Outros applets:
  `NotSupported` (como o `CreateHLEApplet` do Azahar).
- Parâmetro APT genérico (sender, destination, signal, objeto, buffer) que sinaliza o evento de
  parâmetro da aplicação; `GlanceParameter` não consome (só DspSleep/DspWakeup), como o NS.
- O seletor fecha na hora do `StartLibraryApplet` com o MiiResult do Mii padrão do Azahar
  (return_code 0, CRC 0x056C). O Azahar fecha no próximo update (16,7 ms); o jogo espera o evento
  de parâmetro, então dá no mesmo.
- Savestate guarda o parâmetro pendente e o slot (chaves opcionais: savestate antigo abre).
- `MapMemoryBlock`/`UnmapMemoryBlock`: só o bloco do applet ganha regra nova; o caminho do OoT3D
  (other_permissions = DontCare) não muda.
- Recusa do `SendParameter` grava o motivo no nome do evento (foi assim que o item 3 apareceu).
- Testes: `TestAptLibraryAppletMiiSelector` (cada trava vista falhando antes da correção; o
  savestate visto falhando por mutação) e 3 guardas (vistas falhando por mutação, 167/170).

OoT3D: nunca chama Preload/IsRegistered/SendParameter/Glance nem UnmapMemoryBlock (com o perfil
dele, comando sem HLE vira falha nomeada, e ele roda sem isso). O `ReceiveParameter` do Wakeup
segue no caminho antigo.

## Alternativas rejeitadas

- Fazer o stub de `GetAppletInfo`/`IsRegistered` responder "sim" sempre: o jogo depois manda Request
  e espera a Response com o bloco; sem o ciclo, trava na próxima espera.
- Implementar desmapeamento geral na memória do host: mexe na memória que o OoT3D usa a cada
  instrução; o SM3DL só precisa do bloco vazio. Bloco com conteúdo segue como falha nomeada.
- Responder "cancelado" em vez do Mii padrão: o Azahar devolve o Mii padrão, e o jogo pode tratar
  cancelamento como erro.

## Em aberto

- OpenDirectory (`fs:USER` 0x080B0102) e a leitura de diretório: próxima trava.
- `GetAppletInfo` da própria aplicação (0x300) continua sem HLE (o SM3DL não pede).
- `IsRegistered` não conhece os ids "Any*" (0x100/0x200) nem applets de sistema.
- `UnmapMemoryBlock` de bloco com conteúdo (precisa de desmapeamento na memória do host).
- Herdado: UpdateRomFS do SelfNCCH diverge do Azahar; por que um A só não passa do título.
