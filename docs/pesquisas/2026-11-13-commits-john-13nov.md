# Commits do John de 13/11 (fork JohnUzoka/TriAevum-Switch, branch english): o que presta

## Pergunta
O Gustavo pediu para olhar o código novo do John e ver se tem conteúdo que presta, sem trazer nada ainda.
Regra dele (13/11): manter a essência do jogo; a sombra simples perde a originalidade.

## Fontes
`git fetch john; git log --no-merges HEAD..john/english` em integra-john-2 (943cc538): 8 commits, o último às
2026-11-13 05:28 -0500. Diffs lidos com `git show <sha>`; textos de causa/correção em SWITCH_PORT.md de cada commit.

## Conclusão
| Commit | O que é | Veredito |
|---|---|---|
| 8d70ff28 | Câmera livre atravessava pisos com a flag "ignora câmera" (0x2000) do teste de linha do jogo (Forest Temple 78 pisos, 3,2% da área; Spirit, Kokiri, Jabu, Shadow, Hyrule Field menos). Decodifica a colisão estática (GlobalContext+0xA98) uma vez por cena e para o olho 1 unidade acima. Também: rótulos "Item R/L" em Controles > Atalhos. Com testes (mod_profile) e guardas. | Presta: correção de bug de recurso que já temos (câmera livre veio na mescla de 10/11). Não muda o jogo original (câmera livre é opcional). |
| b57d9934 | Log do seletor do D-pad (issue #13: segurar A não fazia nada). | Só diagnóstico; base do 221a425f. |
| 221a425f | Seletor do D-pad passa a usar o "pegar item" do próprio jogo (estado 8 da página de itens, 0x00506748 é o item pego, não o sob o cursor). Junto: sombra suaviza a luz na troca dia/noite. | Parte do seletor presta: o nosso build tem o seletor (veio no 0.2.3, oot3d_top_screen_dpad_items.cpp) e deve ter o mesmo bug #13. Parte da sombra: fora. Commit misto, precisa separar à mão. |
| 694ee0f7 | Sombra sem falhas nos pés; seletor loga as duas posições. | Fora (sombra); o log do seletor é detalhe. |
| ac52389c, 19c480ec, 8229c7ef | Sombra simples em decalque (altura, método Decal no chão/degraus/paredes, só decalque). | Fora: sombra (regra da essência). |
| 199f309f | 0.2.4: sombra ligada e "Preserve scene brightness" ligado por padrão (environments/*.json). | Fora: muda o visual padrão. |

## Decisão
Primeiro nada entraria (Gustavo, 13/11: "não precisa comitar"). Depois, aprovado ("pode ser então") trazer:
- 8d70ff28 inteiro → e35456dd (autor John; guardas 607/607, oot3d_top_screen_mod_profile_tests ok).
- Seletor, depois que o Gustavo confirmou que segurar A falha no nosso build ("falha sim"): b57d9934 → acc809de
  (cherry-pick -x) e só as partes de seletor de 694ee0f7 + 221a425f (dpad_items .cpp/.h/_tests e window_frame) →
  ebb5d35d, com as 2 guardas do seletor (610/610; as duas falham no código anterior). Ficou de fora a troca
  `simpleShadow.TakeFrame()` → `FrameForPublish()` de 694ee0f7, que depende da sombra nova.

## Alternativas rejeitadas
- Trazer o branch inteiro: arrasta a sombra ligada por padrão e o brilho preservado (contra a regra da essência).

## Em aberto
- Teste no console: câmera livre no Forest Temple (não atravessar pisos) e segurar A + D-pad na página de itens.
