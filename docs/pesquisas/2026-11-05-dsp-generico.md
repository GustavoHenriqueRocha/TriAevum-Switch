# Áudio DSP para qualquer título

Data: 2026-11-05 · Títulos: SM3DL, genérico (OoT3D quando indicado) · Origem: workflow ctr-generico-frentes (investigador + verificador adversarial)

## Pergunta

O HLE de DSP do OoT3D serve a qualquer jogo? O que falta?

## Conclusão

O HLE de DSP não depende do OoT e já está ligado para qualquer título. O "não usa" tem duas causas. A primeira: o boot probe sem janela nunca mixa quadros DSP, e isso vale para todo título, o OoT inclusive. A segunda: faltam alguns comandos do serviço dsp::DSP. O comando 0x0013 do Mario é FlushDataCache e cai no stub, que por acaso já responde o mesmo que o Azahar.

(1) Como funciona. São três camadas:
- IPC: oot3d_native_a32_ctr_host_dsp.cpp trata LoadComponent (só guarda os bytes, sem hash), RegisterInterruptEvents (eventos por tipo e canal), GetSemaphoreEventHandle, SetSemaphoreMask, SetSemaphore, WriteProcessPipe, ReadPipeIfPossible, ConvertProcessAddressFromDspDram e GetHeadphoneStatus. No WriteProcessPipe do canal 2 (pipe de áudio), os estados 0, 2 e 3 escrevem no pipe 15 endereços de structs e sinalizam a interrupção (2,2). O estado 1 para a DSP.
- Relógio: AdvanceSystemTicks junta quadros pendentes a cada DspAudioFrameTicks = 160*4096*2. É o mesmo audio_frame_ticks do hle.cpp do Azahar.
- Mixagem: NativeA32DspHle::ProcessFrame (oot3d_native_a32_dsp_hle.cpp) lê as duas regiões de memória compartilhada em 0x1FF50000 e 0x1FF70000 e escolhe a região de leitura pelo frame_counter, como o CurrentRegion do Azahar. Em seguida roda as 24 Source::Tick e o Mixers::Tick do Azahar (copiados em third_party/azahar_audio, com otimizações bit a bit iguais), escreve status e final_samples e devolve 160 quadros estéreo s16 a 32728 Hz. O resolvedor físico (dsp_memory_adapter) só conhece duas regiões: linear heap = físico 0x20000000 e VRAM = 0x18000000.
- Quem chama a mixagem: a janela, em DeliverDspAudioFrames (window_frame.cpp:2462). Para cada quadro pendente: ProcessFrame, SignalDspAudioFrame e RunUntilGuestWait, para a thread de som do jogo rodar. O módulo de jogo faz o mesmo (oot3d_game_module_runtime.cpp:345).

Saída de áudio:
- Linux: o SDLAudioPlayer (runtime/three_ds_recomp/src/ship/audio) abre o dispositivo a 32728 Hz com 160 quadros e AUDIO_S16SYS, no modo fila (SDL_QueueAudio). Quem converte para a taxa da placa é o próprio SDL (hint "high"). A HostAudioStream é opcional (TRIAEVUM_AUDIO_THREAD=1 / TRIAEVUM_AUDIO_STRETCH=1). Existe --audio-pcm-dump para gravar WAV.
- Switch: saída a 48000 Hz (config/audio_sample_rate), passando pelo HostAudioResampler sinc-Kaiser 32728→48000 com ajuste de taxa de ±0,5%. A fila do dispositivo tem pelo menos 4096 quadros e o buffer do dispositivo 1024 (config/audio_device_frames). Toca pelo backend audren do SDL na libnx, com pré-buffer antes de despausar. Por padrão roda numa thread de host própria (HostAudioStream, núcleo 1). Com config/audio_deferred, a mixagem vai para depois da apresentação.

(2) Específico do OoT versus genérico:
- Genérico: todo o protocolo. Os 15 endereços de structs (kDspHleAudioStructAddresses) são idênticos aos do Azahar. Conferi compilando offsetof(SharedMemory, ...) contra o shared_memory.h (sizeof 0x8000), que é igual ao upstream. O Mario pediu e converteu exatamente esses 15 (30 ConvertProcessAddress).
- Específico do OoT, tudo fora do caminho DSP:
  - os núcleos manuais AudioEffect 0x004A022C/0x004A0338 (ManualCompiledFunctions, já desligados no perfil genérico);
  - o motor de som nativo oot3d_native_audio*.cpp, cseq e profiles, com endereços EUR rev0 e fora da janela A32;
  - o parser DSP1 do oot3d_native_audio_resampler.cpp, que é heurístico e sem hash;
  - os caminhos sdmc:/switch/oot3dre/config/audio_*.

(3) O que o Mario fez de verdade (report.json, eventos 419–468):
- A thread 6 abriu dsp::DSP e fez LoadComponent (0xBE44, máscaras 0xff/0xff), RegisterInterruptEvents (2,2), GetSemaphoreEventHandle, SetSemaphoreMask 0x2000, WriteProcessPipe com Initialize, SetSemaphore 0x4000, ReadPipeIfPossible de 2 e de 30 bytes e GetHeadphoneStatus. Tudo tratado e sem stub.
- A thread 3 fez 79 FlushDataCache com endereços do linear heap (até 0xd0d48 bytes, pc 0x1f2fe0). No perfil OoT esse comando daria Fault: o SDK do OoT não o usa.
- A thread 8, a de som, fez 1 WaitSynchronizationN no evento 0x0015802c e nunca acordou em 600 vblanks, porque o probe não chama TakePendingDspAudioFrames.
- Esperado: cerca de 2045 quadros DSP em 600 vblanks (10 s × 32728/160).

(4) Plano, ver os achados:
- primeiro, mixagem DSP no boot probe, com métricas e WAV;
- depois, os comandos que faltam no IPC, conferidos contra o dsp_dsp.cpp do Azahar;
- depois, as transições de estado do hle.cpp (Initialize, Sleep, Wakeup) e reset do NativeA32DspHle;
- depois, contadores de endereço físico inválido, que hoje somem porque LOG_WARNING é ((void)0);
- por último, o pipe binário de AAC.

Não precisa mudar nada no CtrTitleProfile: o DSP já é genérico. No máximo um comentário dizendo isso.

Não mexi em ~/oot3d-work/conv/sm3dl, não rodei build, não fiz commit nem editei C++ ou CMake. Arquivos de apoio no scratchpad:
- /tmp/claude-1000/-home-gustavo/fbbe6f0f-e979-43c8-8f78-34b485dc7a31/scratchpad/sm3dl_svcs.txt (22012 SVCs, uma linha por evento)
- /tmp/claude-1000/-home-gustavo/fbbe6f0f-e979-43c8-8f78-34b485dc7a31/scratchpad/dspcheck/off.cpp (conferência dos offsets)
- /tmp/claude-1000/-home-gustavo/fbbe6f0f-e979-43c8-8f78-34b485dc7a31/scratchpad/azahar/hle.cpp e dsp_dsp.cpp (fontes do Azahar baixadas)

## Achados

Estado de cada um após a verificação: confirmado, parcial (confirmado com ressalva) ou sem suporte.

### O boot probe nunca roda quadros DSP: a thread de som de qualquer título fica parada

Prioridade: alta · Estado: **confirmado**

- **Evidência:** tools/oot3d/native_game_runtime/oot3d_native_a32_boot_probe.cpp:509-536: o laço de vblank faz só AdvanceGuestClockTo, completeGpuWork e SignalVBlank, sem TakePendingDspAudioFrames nem ProcessFrame (grep por dsp/audio no arquivo: zero). No report.json do Mario, a thread 8 fez 1 WaitSynchronizationN:any:0x0015802c=event:guest (evento 468) e nunca mais rodou em 600 vblanks. Esperado: cerca de 2045 quadros DSP. A janela faz isso em oot3d_native_a32_window_frame.cpp:2435-2490 (DeliverDspAudioFrames).
- **Impacto:** Não existe repro sem janela para o áudio de nenhum título. Erros na thread de som (IPC que falta, fault, espera órfã, buffer com endereço inválido) só aparecem no executável com janela, e lá a falha some no meio do resto. O critério '600/600 vblanks' passa com o som morto.
- **Proposta:** No oot3d_native_a32_boot_probe.cpp, entre AdvanceGuestClockTo e SignalVBlank, entregar os quadros como a janela faz: frames = hostServices.TakePendingDspAudioFrames(); para cada um, NativeA32DspHle::ProcessFrame(process.Memory(), pcm) (dspHle construído com BuildNativeA32DspPhysicalRegions(manifest)), depois SignalDspAudioFrame() e result = RunUntilGuestWait(process). Não precisa mexer no CMake: o probe já linka oot3d_native_a32_ctr_host, que inclui dsp_hle.cpp e o azahar_audio. Opções novas: --dsp-audio on|off (padrão on) e --audio-pcm-dump <wav>. Bloco 'dsp' no report.json com frames_mixed, nonzero_samples, peak, pcm_fnv1a64, max_active_sources (contar source_statuses.is_enabled) e wakes da thread que espera o evento (2,2). Teste: no oot3d_native_a32_ctr_host_tests, depois da sequência de DSP que já existe (~linhas 1990-2215), um teste que dê ProcessFrame numa memória com uma fonte PCM16 ligada e cheque PCM diferente de zero e o status escrito na região de escrita. Repro: Mario com --vblanks 1800, esperando frames_mixed ≈ 6136 e thread 8 com mais de 1 espera. Guarda em tests/switch/test_bug_guards.py: o probe precisa chamar TakePendingDspAudioFrames.

- **Verificação:** Conferido. O laço de vblank (oot3d_native_a32_boot_probe.cpp:513-537) só chama AdvanceGuestClockTo, completeGpuWork, SignalVBlank e RunUntilGuestWait; no arquivo não aparece nenhuma referência a dsp ou audio. AdvanceSystemTicks (ctr_host.cpp:1834-1844) acumula mPendingDspAudioFrames, mas ninguém chama TakePendingDspAudioFrames nem SignalDspAudioFrame, e a interrupção (2,2) nunca é sinalizada. No sm3dl_svcs.txt, a thread 8 tem um único evento (468, WaitSynchronizationN no 0x15802c). Esse handle é o evento de interrupção: a thread 6 esperou nele no evento 427, logo depois do WriteProcessPipe, e acordou. O alvo ctr_title_boot_probe linka oot3d_native_a32_ctr_host (host_services.cmake:426-436), que já inclui dsp_hle.cpp, dsp_memory_adapter.cpp e o azahar_audio, e o probe tem o manifest para BuildNativeA32DspPhysicalRegions. A conta de cerca de 2045 quadros (268111856/1310720 ≈ 204,5 por segundo) está certa.

- **Correção do verificador:** A proposta está certa, com estes ajustes:
(a) O laço 'Take → ProcessFrame → SignalDspAudioFrame → RunUntilGuestWait' já existe em dois lugares: window_frame.cpp:2459-2490 e tools/oot3d/native_game_module/oot3d_game_module_runtime.cpp:344-369. O caminho citado no relatório está errado: é native_game_module, não native_game_runtime. Antes de criar uma terceira cópia no probe, extrair uma bomba comum na lib do host, por exemplo DrainDspAudioFrames(host, dspHle, process, sink), e testar essa função no ctr_host_tests.
(b) O teste proposto, de ProcessFrame com uma fonte PCM16, repete o que o oot3d_native_audio_dsp_mix_tests já cobre com os digests kExpected. O teste que falta é o da bomba: quadros pendentes → interrupção (2,2) sinalizada N vezes → waiter acordado.
(c) frames_mixed com --vblanks 1800 deve dar um pouco menos de 6136, porque só conta a partir do Initialize. Reportar também dsp_frames_pending_discarded e o tick do Initialize.
(d) O probe é compilado em -O0 (build_title_boot_probe.sh), então medir o custo de 2000 a 6000 mixagens. O report.json já tem 273 MB e vai crescer com os eventos da thread de som.
(e) O --dsp-audio ligado por padrão também muda a trajetória do probe do OoT (oot3d_native_a32_boot_probe). Não achei baseline que quebre, mas vale anotar no commit.


### dsp::DSP 0x0013 FlushDataCache (e 0x0014 InvalidateDataCache) sem HLE: só funciona pelo stub

Prioridade: alta · Estado: **confirmado**

- **Evidência:** stderr.log do Mario: 'IPC sem HLE: servico="dsp::DSP" comando=0x0013 header=0x00130082 ... pc=0x1f2fe0 thread=3 params=[0x15aa5fe0,0x1198]'. São 79 chamadas (report: 79 'SendSyncRequest:dsp::DSP:sem-HLE'). O caminho é oot3d_native_a32_ctr_host.cpp:664-676: Wait, depois HandleStubServiceRequest, que só existe com StubUnknownServices. No Azahar (src/core/hle/service/dsp/dsp_dsp.cpp:206), FlushDataCache(addr, size, process) devolve MakeBuilder(1,0), ou seja, header 0x00130040 e ResultSuccess. A tabela do Azahar é 0x0013 FlushDataCache e 0x0014 InvalidateDCache.
- **Impacto:** Qualquer jogo de SDK mais novo que o do OoT faz flush dos samples (BCWAV/BCSTM no linear heap) antes de entregar à DSP. No perfil OoT (StubUnknownServices=false) isso vira Fault, TRIAEVUM_HOST_REQUEST_UNSERVED. No genérico funciona por acaso, sujando o log e passando pelo caminho de erro a cada carga de som.
- **Proposta:** Em oot3d_native_a32_ctr_host_internal.h, criar kDspFlushDataCacheRequest=0x00130082/Response=0x00130040 e kDspInvalidateDataCacheRequest=0x00140082/Response=0x00140040. Em HandleDspRequest (oot3d_native_a32_ctr_host_dsp.cpp): ler address e size, conferir o descritor em cmd+12 (kCopyHandleDescriptor) e o handle do processo em cmd+16, responder sucesso e registrar evento ':FlushDataCache:addr=..:size=..'. É no-op correto: a memória guest é memória do host e a mixagem lê na mesma thread do jogo, então não há cache a coerir. Teste em oot3d_native_a32_ctr_host_tests: com o perfil OoT (sem stub), mandar 0x00130082 e exigir Resume, cmd[0]=0x00130040 e cmd[1]=0. Guarda: 0x00130082 presente no ctr_host_dsp.cpp. Repro: no stderr do Mario, as linhas 'servico="dsp::DSP" comando=0x0013' passam de 8 para 0.

- **Verificação:** Os fatos conferem. O stderr.log tem 8 linhas 'dsp::DSP comando=0x0013 header=0x00130082 pc=0x1f2fe0 thread=3', impressas nas chamadas 1, 2, 3, 4, 8, 16, 32 e 64 por causa da compressão do HandleStubServiceRequest (ctr_host.cpp:1696). O sm3dl_svcs.txt tem 79 eventos 'dsp::DSP:sem-HLE:stub'. Os endereços (0x15aa5fe0 etc.) caem no linear heap 0x14000000+64 MiB do process.json. HandleDspRequest não trata o 0x0013: cai no Wait da linha 311, depois no stub de ctr_host.cpp:664-676. O stub responde (cmd<<16)|0x40 = 0x00130040 com resultado 0. É o mesmo que o MakeBuilder(1,0) do Azahar em dsp_dsp.cpp:206-217, e o mesmo vale para o 0x0014 em :219-230. No code.bin do Mario, o literal 0x00140082 aparece em 0x1f3194, dentro do bloco de wrappers DSP: o InvalidateDCache também existe no SDK. Já o impacto e a prioridade estão exagerados. O perfil OoT só é escolhido para os title ids do OoT (ctr_title_profile.h:120-125), então nenhum outro título cai no Fault. O efeito real é ruído no log, 2 eventos por chamada (o evento sem tratamento mais o do stub, 158 no total) e o anel de resumo IPC poluído.

- **Correção do verificador:** Prioridade média ou baixa, não alta. Implementar copiando o padrão que já existe para o gsp::Gpu FlushDataCache (ctr_host_gsp.cpp:12-44):
- descriptor == kCopyHandleDescriptor;
- processHandle == kCurrentProcessHandle;
- size == 0 ou IsMapped(address, size).
De preferência, um helper compartilhado entre GSP e DSP. Também:
- Incluir "dsp::DSP:FlushDataCache" na exclusão de IsIpcSummaryEvent (ctr_host.cpp:93-98), como já se faz para o do gsp.
- A guarda não pode procurar o literal 0x00130082: esse valor já existe em oot3d_native_a32_ctr_host_internal.h:202 como kGspRegisterInterruptRelayQueueResponse. A guarda deve exigir o nome kDspFlushDataCacheRequest dentro de ctr_host_dsp.cpp.
O critério de repro está certo: 8 linhas 'dsp::DSP comando=0x0013' no stderr passam para 0.


### Comandos do protocolo do Azahar que faltam: RecvData, RecvDataIsReady, ReadPipe, GetPipeReadableSize, UnloadComponent, ForceHeadphoneOut

Prioridade: media · Estado: **confirmado**

- **Evidência:** Tabela do Azahar (dsp_dsp.cpp:353-385): 0x0001 RecvData, 0x0002 RecvDataIsReady, 0x000E ReadPipe, 0x000F GetPipeReadableSize, 0x0012 UnloadComponent, 0x0020 ForceHeadphoneOut. O host (ctr_host_dsp.cpp) não trata nenhum deles. No hle.cpp:158-182, RecvData(0) devolve 0 com a DSP ligada e 1 com ela desligada ou dormindo; RecvDataIsReady devolve !sleep_pending. O stub genérico responde zero nos dois: 'não pronto' para sempre.
- **Impacto:** A finalização do som (sair do jogo, soft reset, trocar de modo que reinicia o nn::snd) faz polling em RecvDataIsReady/RecvData(0) depois do Shutdown e entraria em laço infinito. SDKs que leem pelo par GetPipeReadableSize+ReadPipe, em vez de ReadPipeIfPossible, recebem tamanho 0 ou lixo e nunca acham os endereços das structs, e aí o som não inicia.
- **Proposta:** Em HandleDspRequest, seguindo o Azahar:
- RecvData 0x00010040 → 0x00010080: [0]=sucesso, [2]=(mDspAudioRunning?0:1);
- RecvDataIsReady 0x00020040 → 0x00020080: 1;
- GetPipeReadableSize 0x000F0080 → 0x000F0080: tamanho de mDspPipeOutput[canal];
- ReadPipe 0x000E00C0 → 0x000E0042: mesma lógica de buffer estático do ReadPipeIfPossible, mas exigindo dados suficientes;
- UnloadComponent 0x00120000 → 0x00120040: limpa mDspComponent e para o áudio;
- ForceHeadphoneOut 0x00200040 → 0x00200040.
Teste no ctr_host_tests: depois do Initialize, RecvData(0)=0; depois do Shutdown (WriteProcessPipe [1,0,0,0]), RecvData(0)=1; ReadPipe devolve os mesmos 32 bytes do ReadPipeIfPossible. Os dois perfis, sem stub.

- **Verificação:** Conferido em dsp_dsp.cpp:353-385 e hle.cpp:158-182. RecvData devolve 1 com a DSP desligada, dormindo ou com sleep pendente, e 0 com ela ligada. RecvDataIsReady devolve !sleep_pending && sleep_reply_frames==0. Os headers de resposta propostos estão certos: 0x00010080, 0x00020080, 0x000F0080, 0x000E0042, 0x00120040 e 0x00200040. O stub responde cmd[2]=0, que é 'ligada' e 'não pronto': um laço de finalização que espera ready e depois valor 1 nunca termina. Evidência estática nova no code.bin do Mario, procurando os literais de header: 0x00010040 (RecvData) em 0x1f3288 e 0x00020040 (RecvDataIsReady) em 0x1f3038, os dois no bloco de wrappers DSP 0x1f2f..0x1f33, onde estão os PCs dos outros comandos DSP do log. O SDK do Mario tem esses wrappers. 0x000E00C0 (ReadPipe) e 0x00200040 (ForceHeadphoneOut) não aparecem, e nenhum dos dois cabe num imediato ARM, então o Mario não os usa. Os acertos de 0x000F0080 caem fora do bloco DSP: provavelmente são de outro serviço. Para 0x00120000 não dá para concluir, porque cabe num MOV imediato. O impacto 'laço infinito na finalização' é plausível, mas não foi observado.

- **Correção do verificador:** Prioridade: RecvData e RecvDataIsReady primeiro, porque existem no binário do Mario. ReadPipe e ForceHeadphoneOut podem ficar para depois. Ajustes:
- A resposta do ReadPipe não tem palavra de tamanho: é [header 0x000E0042, result, descritor estático (size<<14)|2, endereço].
- Pelo Azahar, ReadPipe com dados insuficientes é UNREACHABLE (o hardware trava). Melhor registrar Fault nomeado do que devolver lixo.
- RecvDataIsReady=1 fixo só vale enquanto o Sleep do host for síncrono. Se o achado 4 implementar o sleep_pending atrasado, as duas respostas precisam seguir sleep_pending e sleep_reply_frames.
- Script de conferência: /tmp/claude-1000/-home-gustavo/fbbe6f0f-e979-43c8-8f78-34b485dc7a31/scratchpad/dsphdr.py (faz grep dos headers IPC no code.bin).


### Transições de estado do pipe de áudio diferentes do hle.cpp do Azahar (Initialize, Sleep, Wakeup)

Prioridade: media · Estado: **confirmado**

- **Evidência:** ctr_host_dsp.cpp:148-178: os estados 0, 2 e 3 só reescrevem os endereços e sinalizam; o 3 (Sleep) ainda desliga o áudio na hora. No Azahar (hle.cpp:228-278 e 315-361), Initialize zera dsp_memory, chama mixers.Reset(), Source::Reset() em todas, aac Reset e ResetPipes. Sleep marca sleep_pending; no tick seguinte salva a DSP RAM e as fontes, escreve os endereços e passa a Sleeping; 2 quadros depois sinaliza a interrupção (Zero, Debug). Wakeup restaura a memória e as fontes. Estado desconhecido vira Off. O NativeA32DspHle não tem Reset() e o host nem tem acesso a ele: o objeto mora na sessão da janela ou do módulo.
- **Impacto:** Um jogo que reinicializa a DSP (soft reset, voltar ao título, sair e voltar do HOME ou da suspensão) continua com vozes, histórico ADPCM e estado dos mixers da sessão anterior: estalos, vozes fantasmas, ou espera eterna pela interrupção (Zero) do Sleep.
- **Proposta:** No host, um contador mDspHleResetGeneration (mais um pedido de Sleep/Wakeup) incrementado no Initialize e lido pela janela, pelo probe e pelo módulo antes do próximo ProcessFrame. Em NativeA32DspHle, criar Reset() (recria Sources e Mixers, zera as duas regiões 0x1FF50000/0x1FF70000) e Sleep/Wakeup com backup da DSP RAM, como no Azahar. Sleep deixa de sinalizar na hora e passa a sinalizar no tick seguinte, mais a interrupção (0, canal 0) dois quadros depois. Incluir os campos novos no CaptureState e no RestoreState do host (ctr_host_state.cpp:199-208) e no schema do DSP HLE (v2). Teste: Initialize, ProcessFrame com uma fonte tocando, Initialize de novo, ProcessFrame com saída zero e source_statuses zerados.

- **Verificação:** Conferido. ctr_host_dsp.cpp:151-185 trata os estados 0, 2 e 3 do mesmo jeito: reescreve os endereços, sinaliza (2,2) e, no 3, desliga na hora. Não há reset das Sources nem dos Mixers. NativeA32DspHle (dsp_hle.h) não tem Reset e mora em dspHleStorage, na sessão da janela, sem acesso pelo host. No Azahar (hle.cpp:228-278 e 315-361), Initialize zera a DSP RAM inteira, reseta mixers, fontes, AAC e todos os pipes. Sleep fica pendente até o próximo tick, e dois ticks depois vem a interrupção (Zero, Debug). Estado desconhecido vira Off.

- **Correção do verificador:** Faltam detalhes na proposta:
(a) O Initialize do Azahar faz dsp_memory->raw_memory.fill(0), ou seja, zera os 512 KiB da DSP RAM. Aqui isso é a região guest ctr_dsp_ram em 0x1FF00000, que o próprio host consegue zerar dentro do HandleDspRequest, sem precisar do NativeA32DspHle. Também é preciso limpar todos os mDspPipeOutput, não só o 2.
(b) No Azahar, InterruptType::Zero e One são eventos únicos, que não dependem do canal (GetInterruptEvent, dsp_dsp.cpp:318-333). O host indexa [type][channel]. A resposta ao Sleep deve sinalizar o evento tipo 0 registrado em qualquer canal, ou então normalizar o canal para 0 no RegisterInterruptEvents dos tipos 0 e 1.
(c) O 'dois quadros depois' precisa de relógio. Hoje o estado 3 zera mNextDspAudioFrameTick e o AdvanceSystemTicks para de contar. É preciso manter o tick rodando no estado Sleeping, sem mixar (o Tick do Azahar só mixa com On), só para contar sleep_reply_frames.
(d) No Switch não existe tampa. O gatilho real é Shutdown seguido de Initialize (soft reset ou reinício do nn::snd): prioridade média para baixa.


### Pipe binário (canal 3, decodificador AAC) sem HLE: o jogo espera uma resposta que nunca vem

Prioridade: media · Estado: **confirmado**

- **Evidência:** ctr_host_dsp.cpp:136-200: WriteProcessPipe aceita qualquer canal menor que 8 e só trata o canal 2. O canal 3 recebe sucesso, mas nada é escrito em mDspPipeOutput[3] e não há interrupção. No Azahar (hle.cpp:280-299), DspPipe::Binary lê HLE::BinaryMessage, chama aac_decoder->ProcessRequest, escreve a resposta no pipe 3 e sinaliza (Pipe, Binary). Além disso, AudioTickCallback não sinaliza o Binary por quadro no upstream atual.
- **Impacto:** Título que decodifica AAC pela DSP (música em stream ou vídeo com áudio AAC) trava a thread do decodificador esperando o evento (2,3). Com KernelWaitTimeouts, pode só ficar mudo. Não aparece no Mario até agora: só o canal 2 foi usado.
- **Proposta:** Curto prazo: no perfil genérico, registrar no stderr 'TRIAEVUM_DSP_BINARY_PIPE codec=..' e responder BinaryMessage com result de erro (como o Azahar faz para codec desconhecido), sinalizando (2,3) para o jogo não ficar preso. Médio prazo: portar audio_core/hle/aac_decoder.cpp e decoder.h do Azahar para third_party/azahar_audio, com o backend faad2 que o Azahar usa (conferir licença e build no devkitA64), e ler o PCM via resolvedor físico. Teste: WriteProcessPipe canal 3 com um BinaryMessage de Init deve deixar 32 bytes de resposta no pipe 3 e sinalizar o evento (2,3).

- **Verificação:** O comportamento do host confere. WriteProcessPipe aceita canal < 8 (ctr_host_dsp.cpp:137) e só trata o canal 2. Nos outros, validPayload continua true: responde sucesso, sem saída e sem interrupção. No Azahar (hle.cpp:280-299), o Binary exige exatamente 32 bytes e codec DecodeAAC, chama aac_decoder->ProcessRequest, escreve a resposta no pipe 3 e sinaliza (Pipe, Binary). O AudioTickCallback só sinaliza Pipe/Audio por quadro (hle.cpp:503-519). Mas a afirmação 'responder com erro, como o Azahar faz para codec desconhecido' está errada.

- **Correção do verificador:** Com tamanho errado ou codec diferente de DecodeAAC, o Azahar faz LOG_CRITICAL + UNIMPLEMENTED e retorna sem resposta e sem interrupção. A resposta com ResultStatus::Error só sai do AACDecoder::ProcessRequest, para comando desconhecido dentro do DecodeAAC (aac_decoder.cpp:21-62). Responder com erro e sinalizar (2,3) é uma escolha defensiva do TriAevum, não paridade com o Azahar; vale documentar assim. A resposta deve repetir codec e cmd do pedido, com result=Error, e ter exatamente 32 bytes. Licença: o faad2 é GPLv2+ e o TriAevum é GPLv3 (LICENSE), então são compatíveis. Lembrar que o decoder do Azahar lê e escreve PCM por endereço físico de FCRAM e precisaria do mesmo resolvedor. Os pipes 0, 1 e 4 a 7 também respondem sucesso mudo, enquanto no Azahar dão UNIMPLEMENTED: registrar no log.


### A mixagem descarta buffers inválidos em silêncio; o resolvedor físico só cobre linear heap e VRAM

Prioridade: media · Estado: **confirmado**

- **Evidência:** third_party/azahar_audio/common/logging/log.h:5-6 define LOG_WARNING e LOG_ERROR como ((void)0). Em source.cpp:686-730, se o memory_resolver devolve nullptr, o buffer é limpo e pulado sem nenhum sinal; formato desconhecido cai em UNIMPLEMENTED(). O dsp_memory_adapter.cpp:9-21 só mapeia 0x20000000→LinearHeapBaseAddress (tamanho do manifesto, 64 MiB no Mario) e 0x18000000→ctr_vram. O Memory::GetPhysicalPointer do Azahar cobre FCRAM inteira, VRAM, DSP RAM e a FCRAM extra do N3DS.
- **Impacto:** Num título novo, 'sem som' pode ser endereço físico fora das regiões (buffer em DSP RAM, linear heap maior que o manifesto, layout de memória do N3DS), e o runtime não deixa rastro. Exatamente o tipo de falha que custa horas de caça a fantasma.
- **Proposta:** Em NativeA32DspHle::Impl, envolver o resolvedor num contador: misses por fonte, último endereço inválido e formato inválido. Expor isso em CaptureStats() e levar ao dspStats da janela e ao bloco 'dsp' do probe. Somar a região da DSP RAM (físico 0x1FF00000 → kDspRamVirtualAddress, 512 KiB) e, se o manifesto expuser, a FCRAM fora do linear heap. Teste no oot3d_native_audio_dsp_mix_tests: fonte com physical_address 0x10000000 deve dar resolver_misses=1 e saída zero, sem mudar os digests de referência (kExpected) para fontes válidas.

- **Verificação:** Conferido:
- third_party/azahar_audio/common/logging/log.h define LOG_WARNING e LOG_ERROR como ((void)0), e common/assert.h:8 faz o mesmo com UNIMPLEMENTED().
- source.cpp:686-730: quando o resolvedor devolve nullptr, o buffer é limpo sem nenhum sinal.
- dsp_memory_adapter.cpp:8-21 só mapeia 0x20000000 → LinearHeapBaseAddress (0x14000000, 64 MiB no process.json do Mario) e 0x18000000 → ctr_vram.
- O manifesto do Mario tem ctr_dsp_ram em 0x1FF00000 (512 KiB), que fica de fora.

- **Correção do verificador:** A proposta está certa. Acrescentar:
- contar também os UNIMPLEMENTED_MSG de atualização parcial de buffer PCM8/ADPCM (source.cpp:482 e 493) e o formato inválido (source.cpp:723), e não só os misses do resolvedor;
- para o físico 0x1FF00000, usar o endereço da system region ctr_dsp_ram do manifesto, como já se faz com a VRAM, e não uma constante;
- o RestoreState confere PhysicalRegions por igualdade (dsp_hle.cpp:179-190), então acrescentar uma região quebra savestates antigos: subir o schema ou aceitar o layout antigo.


### O perfil genérico não desliga nada do DSP; o que é do OoT está fora desse caminho

Prioridade: baixa · Estado: **confirmado**

- **Evidência:** ctr_title_profile.h não tem nenhuma chave de áudio. A sessão dsp::DSP é aberta para todos (ctr_host.cpp:1248) e roteada a HandleDspRequest (ctr_host.cpp:1176). A janela cria o dspHle e entrega quadros sem olhar genericTitle (window_setup.cpp:1218 e window_frame.cpp:2435). Partes do OoT: kAudioEffectProcessFourChannelDelay 0x004A022C e kAudioEffectProcessStereoReverb 0x004A0338 em oot3d_native_compiled_functions.cpp:42-43, desligadas por ManualCompiledFunctions=false (window_setup.cpp:582); oot3d_native_audio*.cpp, cseq e audio_profiles (Oot3dEurRev0AudioEnvelopeLayout), que entram só no native_runtime.cmake e não na janela A32; config em sdmc:/switch/oot3dre/config/audio_*. Endereços de structs: conferidos iguais aos do Azahar via offsetof (0xBFFF, 0x9E92, ..., 0xAC5C). LoadComponent não confere hash de firmware.
- **Impacto:** Não há trava escondida: o executável com janela do Mario já deve gerar quadros DSP e acordar a thread de som. O risco é atribuir 'sem som' ao perfil quando a causa está nos achados de protocolo e de memória.
- **Proposta:** Pôr no CtrTitleProfile um comentário dizendo que o DSP HLE é sempre ligado e genérico. Criar uma seção no docs/GUIA_PORT_3DS_SWITCH.md, 'DSP HLE para outro título', com:
- a sequência de IPC esperada do SDK (a do Mario como referência);
- como ler o bloco 'dsp' do probe;
- a tabela dos comandos tratados contra o Azahar.
Criar guardas em tests/switch/test_bug_guards.py ('perfil genérico não pode condicionar DeliverDspAudioFrames a genericTitle'; 'FlushDataCache tratado sem stub'). Os caminhos de config do Switch (oot3dre/config/audio_*) ficam para quando houver um produto por título.

- **Verificação:** Conferido:
- ctr_title_profile.h não tem nenhuma chave de áudio.
- dsp::DSP entra na lista do GetServiceHandle (ctr_host.cpp:1247-1249) e é roteado em ctr_host.cpp:1176.
- dspHle é criado sem condição em window_setup.cpp:1237-1238, não na 1218 citada pelo relatório.
- DeliverDspAudioFrames é chamado sem olhar genericTitle (window_frame.cpp:2435-2443).
- Os núcleos AudioEffect 0x004A022C e 0x004A0338 (compiled_functions.cpp:42-43) ficam desligados em window_setup.cpp:583-589.
- Os 15 endereços de struct batem: rodei dspcheck/off, que dá 0xBFFF..0xAC5C e sizeof 0x8000, igual a kDspHleAudioStructAddresses e às 30 ConvertProcessAddress do Mario.
Correções menores: o módulo fica em tools/oot3d/native_game_module/oot3d_game_module_runtime.cpp:344. No Switch, config/audio_deferred, audio_thread e audio_stretch vêm ligados quando o arquivo não existe (window_setup.cpp:98-112 e 1409-1416); o resumo dava a entender que eram opt-in.

- **Correção do verificador:** Acrescentar ao 'específico do OoT versus genérico' uma diferença genérica que o relatório não viu. A cópia do azahar_audio é da revisão beb5681e (docs/AZAHAR_AUDIO_PROVENANCE.md). O Azahar atual tem audio_core/hle/effects.cpp, com DelayEffect implementado e ReverbEffect ainda stub, e o Mixers::Tick chama ApplyEffects(). O mixers.cpp vendorizado não tem nada disso. Um título que use o delay da DSP (delay_effect_*_dirty em DspConfiguration) toca diferente no TriAevum e no Azahar. Isso entra como item no plano, com o mesmo cuidado bit a bit dos digests do dsp_mix_tests.


## Em aberto

- Não verifiquei de ponta a ponta se a thread de som do Mario (thread 8) roda sem fault quando os quadros DSP chegam. Isso depende do achado 1 (probe) ou do executável com janela que o outro agente está construindo; vale olhar o dspStats (FramesMixed, NonzeroSamples) no resumo da janela.
- Não sei se o SDK do Mario usa RecvData/RecvDataIsReady na finalização, nem se algum SDK usa ReadPipe/GetPipeReadableSize em vez de ReadPipeIfPossible. Afirmei pela semântica do Azahar e do 3dbrew; só confirma com um probe longo que inclua soft reset ou saída.
- Quais títulos usam o pipe binário (AAC) e se o faad2 compila no devkitA64 com a licença aceitável: não levantado.
- O semaphore event da DSP nunca é sinalizado, nem aqui nem no Azahar (stub). Se algum título esperar nele sem timeout, vai travar. Não vi isso no Mario: ele só pegou o handle.
- Não comparei o tamanho do componente DSP do OoT com o do Mario (0xBE44). Irrelevante para o HLE, mas útil como telemetria por título.
- O custo de CPU da thread de som do Mario por quadro (o OoT gasta ~1,2 ms no Switch, tour23) é desconhecido. Sem os núcleos manuais AudioEffect, um título com efeitos pesados na thread de som pode pesar no frame time.
- (verificador) docs/AZAHAR_AUDIO_PROVENANCE.md não foi consultado. A cópia vendorizada (Azahar beb5681e) não tem o effects.cpp do upstream (DelayEffect implementado) nem o Mixers::ApplyEffects: efeitos de delay da DSP são ignorados em qualquer título.
- (verificador) O stub genérico também esconde validações que falham em comandos DSP conhecidos. HandleDspRequest devolve Wait em qualquer checagem que falha, por exemplo ReadPipeIfPossible com buffer estático de índice diferente de 0, WriteProcessPipe com estado inválido ou LoadComponent com descritor diferente. No perfil genérico isso vira 'IPC sem HLE' com sucesso e resposta zerada, o que confunde: parece comando desconhecido quando é falha de validação. Precisa de log próprio ('comando conhecido, validação falhou').
- (verificador) Interrupções tipo 0 e 1 no Azahar são eventos únicos, independentes do canal, e há limite de eventos registrados (HasTooManyEventsRegistered). O host indexa [tipo][canal] sem limite.
- (verificador) Evidência estática barata não foi usada. Procurar os literais de header IPC no code.bin do Mario mostra RecvData (0x1f3288), RecvDataIsReady (0x1f3038) e InvalidateDCache (0x1f3194) no bloco de wrappers DSP, e ReadPipe e ForceHeadphoneOut ausentes. Script: /tmp/claude-1000/-home-gustavo/fbbe6f0f-e979-43c8-8f78-34b485dc7a31/scratchpad/dsphdr.py.
- (verificador) O laço de entrega DSP já existe duplicado (janela e módulo). O probe seria a terceira cópia: falta propor extrair uma bomba comum na lib do host.
- (verificador) IsIpcSummaryEvent (ctr_host.cpp:93-98) exclui só o FlushDataCache do gsp. O da DSP (79 na carga e mais durante o jogo) vai inundar o anel de resumo IPC quando for tratado.
- (verificador) Cada chamada que cai no stub gera 2 eventos SVC (o sem tratamento mais o do stub). As 79 FlushDataCache viraram 158 eventos, o que distorce contagens do report.
- (verificador) A janela entrega os quadros DSP em rajadas por refresh (3 a 4 de uma vez antes do SignalVBlank), não a cada 4,9 ms. O probe proposto herda isso. Não foi avaliado se algum título depende do ritmo intercalado.
- (verificador) O relatório não verificou se o Ship::Context e o audioPlayer sobem para um título genérico na janela (caminhos sdmc:/switch/oot3dre/config, inicialização do SDL). Sem isso, o dspStats pode mixar e não tocar nada.
- (verificador) Pipes 0, 1 e 4 a 7: o host responde sucesso mudo, enquanto o Azahar dá UNIMPLEMENTED. Também não foi notado que WriteProcessPipe aceita payload com mais de 4 bytes no canal de áudio (o Azahar exige exatamente 4).
- (verificador) Custo da mixagem no probe -O0 e crescimento do report.json (já com 273 MB) quando a thread de som passar a rodar: nada disso foi estimado.
- (verificador) RestoreState do NativeA32DspHle exige PhysicalRegions idênticas: acrescentar a região da DSP RAM (achado 6) quebra savestates antigos se o schema não subir.
