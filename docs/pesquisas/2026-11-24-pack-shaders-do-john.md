# OoT3D: shaders do pacote do John no nosso deko3d.pack (24/11)

## Pergunta

As issues #18 (Barinade, Tumba Real) e #19 (Templo do Fogo, flashback da Zelda) do fork do John
(JohnUzoka/TriAevum-Switch) eram shaders fora do pacote, que caem no programa uber. O nosso pacote foi
coletado só pelo nosso tour. Dá para aproveitar o pacote dele sem jogar essas áreas?

## Fontes

- Release `v0.2.8-test.4` do John (21/11): `deko3d.pack` (sha256 211ae939..., 26180864 bytes) e
  `deko3d_reference.pack` (6b97a7c7...), conferidos contra o `SHA256SUMS.txt` da release.
- Issues #18 e #19: "24 saved fragment shaders missing from the 0.2.7 pack" e "85 fragment shader sources
  missing from 0.2.7", incluídos na 0.2.8-test.1.
- Formato: `runtime/three_ds_recomp/include/fast/backends/gfx_deko3d_shader_pack.h` (DKP1 v1; a palavra do
  cabeçalho é `flags`, bit 1 = `kShaderPackFlagTevPassthroughSkip`); junção com
  `tools/oot3d/deko3d_shader_pack.py --pack <john> --pack <nosso> -o deko3d.pack`.
- Carregamento: `gfx_deko3d_shaders.cpp` (`ReadWholeFile`, o pacote inteiro vai para a memória, sem limite).

## Conclusão

- Mesmo formato e mesma chave (FNV-1a do estágio + texto). O pacote do John tem 3141 entradas, o nosso 2255;
  601 em comum, 2540 só dele. Entradas são só DKSH: chave que o nosso runtime nunca gerar fica sem uso.
- Pacote junto: 4795 shaders (2 vertex, 4793 fragment), 35876096 bytes (md5 8247b5bb...), flags 1
  (2509 entradas com os estágios de passagem do TEV pulados, transformação exata). As 2255 nossas ficaram
  idênticas (o nosso vem por último e vence nas chaves comuns).
- Custo: o pacote em memória passa de 11,2 para 34,2 MiB.

## Decisão e alternativas

- Usar o pacote junto; o anterior fica como `shadercache/deko3d.antes-john.pack` (SD e
  `~/oot3d-work/stage-1.0.2`).
- Rejeitado: jogar cada área lenta para coletar os faltantes (é o que o John já fez com os logs dos usuários).
- Rejeitado: só o `deko3d_reference.pack` dele (mesmas chaves, sem o pulo do TEV; o nosso runtime lê os dois).

## Em aberto

- Quantas das 2540 chaves o nosso runtime gera de fato (o fork dele mudou shaders, como a sombra simples):
  medir pelos faltantes em `logs/deko3d_shaders/` e pelo GPU no Templo do Fogo e no Barinade.
- Se os 23 MiB a mais pesam no boot ou na memória do jogo.
