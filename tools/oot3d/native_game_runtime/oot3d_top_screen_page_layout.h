#pragma once

// Onde o TopScreen desenha as páginas 320x240 da tela de baixo (pausa Items,
// Gear, Map, System; seleção de arquivo, entrada de nome) na tela de cima de
// 400x240, e o inverso: o X de toque da tela de baixo sob um X da tela de cima.
// Os planos de alvo de página, o desenho alargado de Items e o mapeamento de
// toque leem isto, então o que é desenhado e o que um toque seleciona não divergem.
//
// O X da tela de cima é a coluna apresentada em pixels nativos (0..400). O
// renderizador põe a tela inteira de 400x240 na saída (encaixe centralizado,
// FitUiPresentation); este header cobre só a página dentro dela.

#include <cstdint>

namespace Oot3dNativeGame {

inline constexpr float kTopScreenPresentedWidth = 400.0F;
// A tela de baixo, que também é o domínio de toque nativo.
inline constexpr float kTopScreenPageWidth = 320.0F;

// Página centralizada: o TopScreen realoca uma página da tela de baixo no
// viewport do alvo de cima (0, 40, 480, 320); o Y do alvo rotacionado é o
// X apresentado, então o X de página 0..320 cai no X 40..360 da tela de cima,
// um pixel nativo por pixel de página (a ortográfica de 320 de largura da página).
inline constexpr std::uint32_t kTopScreenCentredPageX = 40U;
inline constexpr std::uint32_t kTopScreenPresentedWidthPixels = 400U;
inline constexpr std::uint32_t kTopScreenPageWidthPixels = 320U;
static_assert(kTopScreenCentredPageX * 2U + kTopScreenPageWidthPixels ==
              kTopScreenPresentedWidthPixels);

// Páginas largas (2.1.1 0x005D32A4: Items, Map de dungeon, System) ocupam todo o
// viewport (0, 0, 480, 400) e a linha de projeção do payload
// clip = kTopScreenWidePageScale * x + offset (0x005CACA0), que põe o X de
// página no X 200 * (1 - offset) + x da tela de cima.
inline constexpr float kTopScreenWidePageScale = -0.005F;      // 0xBBA3D70A
inline constexpr float kTopScreenWidePageOffset = 0.8F;        // 0x3F4CCCCD
inline constexpr float kTopScreenWideItemsOffset = 0.6F;       // 0x3F19999A
inline constexpr float kTopScreenWideItemsCursorOffset = 0.685F; // 0x3F2F5C29

[[nodiscard]] constexpr float TopScreenWidePageX(float offset) noexcept {
  return (1.0F - offset) * kTopScreenPresentedWidth * 0.5F;
}

// Um pixel de página por pixel apresentado, como na página centralizada.
static_assert(kTopScreenWidePageScale * kTopScreenPresentedWidth * 0.5F > -1.0001F &&
              kTopScreenWidePageScale * kTopScreenPresentedWidth * 0.5F < -0.9999F);
// O offset de página reproduz a página centralizada (X + 40); Map de dungeon e
// System, portanto, mantêm o layout centralizado.
static_assert(TopScreenWidePageX(kTopScreenWidePageOffset) >
                  kTopScreenCentredPageX - 0.001F &&
              TopScreenWidePageX(kTopScreenWidePageOffset) <
                  kTopScreenCentredPageX + 0.001F);

// A página Items alargada (0x005D19F8): seu desenho usa o offset 0.6 (X + 80),
// e nos model streams da página os quads à esquerda de X 256 se movem antes: as
// colunas de fundo em -40 (X + 40, onde a página centralizada as tinha), a
// grade de itens e seus ícones em -17 (X + 63, o 0.685 do cursor da grade). O que
// começa em X 256 ou depois, a sexta coluna especial, fica em X + 80.
inline constexpr float kTopScreenWideItemsLeftRegionEnd = 256.0F;
inline constexpr float kTopScreenWideItemsBackgroundShift = -40.0F;
inline constexpr float kTopScreenWideItemsGridShift = -17.0F;

static_assert(TopScreenWidePageX(kTopScreenWideItemsOffset) +
                      kTopScreenWideItemsGridShift >
                  TopScreenWidePageX(kTopScreenWideItemsCursorOffset) - 0.001F &&
              TopScreenWidePageX(kTopScreenWideItemsOffset) +
                      kTopScreenWideItemsGridShift <
                  TopScreenWidePageX(kTopScreenWideItemsCursorOffset) + 0.001F);

enum class TopScreenPageLayout : std::uint8_t {
  // Páginas centralizadas, os menus de frontend realocados, e o Map de dungeon e
  // System largos (cujo offset 0.8 é o posicionamento centralizado).
  Centred,
  // A página Items alargada.
  WideItems,
};

// X da tela de cima do conteúdo interativo no X de página (toque).
[[nodiscard]] constexpr float TopScreenPageToPresentedX(
    float pageX, TopScreenPageLayout layout) noexcept {
  if (layout == TopScreenPageLayout::Centred)
    return static_cast<float>(kTopScreenCentredPageX) + pageX;
  const float items = TopScreenWidePageX(kTopScreenWideItemsOffset);
  return pageX < kTopScreenWideItemsLeftRegionEnd
             ? items + kTopScreenWideItemsGridShift + pageX
             : items + pageX;
}

// O inverso: o X de página (toque) sob um X da tela de cima. Na página Items
// alargada, um ponto entre a região da grade e a sexta coluna (X da tela de cima
// 319..336, onde nenhuma é desenhada) mapeia para X 256, também entre elas.
[[nodiscard]] constexpr float TopScreenPresentedToPageX(
    float presentedX, TopScreenPageLayout layout) noexcept {
  if (layout == TopScreenPageLayout::Centred)
    return presentedX - static_cast<float>(kTopScreenCentredPageX);
  const float items = TopScreenWidePageX(kTopScreenWideItemsOffset);
  const float grid = presentedX - (items + kTopScreenWideItemsGridShift);
  if (grid < kTopScreenWideItemsLeftRegionEnd)
    return grid;
  const float right = presentedX - items;
  return right < kTopScreenWideItemsLeftRegionEnd
             ? kTopScreenWideItemsLeftRegionEnd
             : right;
}

} // namespace Oot3dNativeGame
