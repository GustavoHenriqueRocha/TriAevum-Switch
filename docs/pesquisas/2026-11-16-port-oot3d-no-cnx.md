# Port do OoT3D no pack CNX (16/11)

## Pergunta
O Gustavo soube que "entrou um port do Ocarina 3DS no CNX pack". Do que se trata?

## Fontes
- Pack: github.com/CostelaCNX/CNX, release 23.5.0-2 (15/11). Notas das 3 últimas releases não citam
  Ocarina; `unzip -l CNX.23.5.0-2.zip` (621 linhas) não tem nada de OoT/TriAevum.
- Os ports chegam pelo CNX Updater (aba "Ports"), não pelo zip: issues #253, #254, #268 do CNX.
  O autor diz em #254/#268 que não faz port nem lista: o updater só mostra um catálogo externo.
- `strings switch/cnx-updater/cnx-updater.nro`: o catálogo é `https://portnx.costelabr.com.br/index.tfl`,
  índice no formato Tinfoil criptografado (cabeçalho `TINFOIL`, 31232 B em 16/11). Não foi aberto.
- Web (busca "Ocarina of Time 3D Switch port homebrew NRO"): o único port do OoT3D para Switch 1 que
  aparece é o JohnUzoka/TriAevum-Switch (o fork em inglês do John). v0.2.6 de 14/11, 3041 downloads
  somados nos assets, 33 estrelas (`gh api repos/JohnUzoka/TriAevum-Switch/releases/latest`).
  O README dele credita GustavoHenriqueRocha/TriAevum-Switch na linha 2 e tem a seção "Credit Where It Belongs".

## Conclusão
Sem confirmação direta: o catálogo do CNX é criptografado. Pela data (onda de ports em out-nov, #254)
e por ser o único port público, o item do CNX é quase certamente o TriAevum-Switch do John, que parte do
nosso port. Precedente: #268 (Midnight Club), em que o autor do port pediu crédito e o CostelaCNX disse que
ia falar com quem mantém a lista.

## Em aberto
- Ver no console (CNX Updater → Ports) o nome, a versão e a descrição do item, e se ele dá crédito.
- Decidir se vale pedir crédito como no #268 (só se o Gustavo quiser).
