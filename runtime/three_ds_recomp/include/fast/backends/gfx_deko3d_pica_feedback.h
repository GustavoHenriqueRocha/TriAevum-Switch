#pragma once

// Superfícies que a GPU escreveu, amostradas como textura (deko3d). Ideia do
// upstream 1757f77c ("GPU-produced surfaces for tiled transfers and sampled
// colour targets"), sem o livro-razão de cópias do Vulkan: a abertura do MM3D
// (Tael, rastro e brilho) e a Floresta Perdida reduzem a cena com um
// DisplayTransfer tiled->tiled (2x2) para memória de textura e fazem um bloom
// em ping-pong entre alvos pequenos que se amostram. A GPU nunca escreve a
// memória do guest nesses endereços (93627fa4: hoje lêem preto transparente).
//
// Decisão pura (testada no host, oot3d_deko3d_backend_tests): qual imagem da
// GPU serve a textura e como copiá-la para uma imagem na orientação das
// texturas decodificadas. O lado do console é gfx_deko3d_pica_draw.cpp.

#include "fast/renderer3ds/pica_native_state.h"

#include <cstdint>

namespace Fast::Deko3d {

// Um DisplayTransfer cuja saída tem o layout de uma textura: tiled->tiled
// (dont_swizzle, bit 5, sem entrada linear), sem inversão vertical (bit 0),
// sem ser TextureCopy crua (bit 3) nem block 32 (bit 16), escala 0..2. Mesmo
// critério do PlanPicaTiledTransferResult do upstream. As apresentações
// (tiled->linear, 01001004/00001004) não são texturas.
[[nodiscard]] constexpr bool PicaTransferWritesTexture(uint32_t flags) noexcept {
    return (flags & (1U | 2U | 8U | 0x10000U)) == 0U && (flags & 0x20U) != 0U && ((flags >> 24U) & 3U) <= 2U;
}

// Formato de cor da saída de um DisplayTransfer (bits 12-14).
[[nodiscard]] constexpr uint8_t PicaTransferOutputFormat(uint32_t flags) noexcept {
    return static_cast<uint8_t>((flags >> 12U) & 7U);
}

// Guest: decodifica a memória (nada da GPU ali). Clear: o preto transparente
// de 93627fa4 (a GPU escreveu, mas esta imagem não serve). Target/Display: a
// imagem da GPU, copiada para a imagem derivada do plano.
enum class PicaFeedbackSource : uint8_t { Guest, Clear, Target, Display };

// Um produtor no endereço da textura (mesmo namespace): o alvo de cor com esse
// endereço ou a imagem de exibição da última transferência para ele.
struct PicaFeedbackProducer {
    bool Present = false;
    bool TextureLayout = true; // alvo: sempre; transferência: PicaTransferWritesTexture
    uint8_t ColorFormat = 0;   // formato PICA (0 = RGBA8)
    uint32_t NativeWidth = 0;  // extensão nativa (linha de tiles = NativeWidth)
    uint32_t NativeHeight = 0;
    uint32_t AllocWidth = 0;   // imagem do host
    uint32_t AllocHeight = 0;
    uint32_t ContentWidth = 0; // pixels do host com o conteúdo (canto da linha 0; < Alloc na resolução dinâmica)
    uint32_t ContentHeight = 0;
    uint64_t WriteSerial = 0;  // ordem da última escrita de cor da GPU (0 = nunca)
};

struct PicaFeedbackPlan {
    PicaFeedbackSource Source = PicaFeedbackSource::Guest;
    const char* Reason = nullptr; // por que Clear
    // Imagem derivada: a textura inteira na escala de alocação do produtor;
    // linha 0 = começo da memória, como as texturas decodificadas (Detile).
    uint32_t ImageWidth = 0;
    uint32_t ImageHeight = 0;
    uint32_t SourceWidth = 0; // retângulo lido do produtor (a partir de 0,0)
    uint32_t SourceHeight = 0;
    uint32_t DestWidth = 0;   // retângulo escrito na derivada (a partir de 0,0)
    uint32_t DestHeight = 0;
    bool FlipY = false;       // a cópia inverte as linhas
    bool Linear = false;      // filtro linear (conteúdo menor que o destino)
    bool CoversImage = false; // falso: o resto da derivada fica preto transparente
    uint64_t WriteSerial = 0; // identidade da versão copiada
};

// enabled: perfil de título (GpuFeedbackTextures). viewportFlipY: o viewport
// do deko3d põe NDC -1 (o fundo do quadro PICA, última linha da memória) na
// linha 0 do alvo (config/deko3d_flip_y, padrão ligado); a textura decodificada
// tem a primeira linha da memória na linha 0 e o shader lê (s, 1 - t), como
// o Citra (t invertido na memória), então a cópia do alvo inverte as linhas.
[[nodiscard]] inline PicaFeedbackPlan PlanPicaFeedbackTexture(const Renderer3ds::PicaTextureView& texture,
                                                              const PicaFeedbackProducer& target,
                                                              const PicaFeedbackProducer& display, bool enabled,
                                                              bool viewportFlipY) noexcept {
    PicaFeedbackPlan plan;
    if (!target.Present && !display.Present) {
        return plan;
    }
    plan.Source = PicaFeedbackSource::Clear;
    const auto clear = [&](const char* why) {
        plan.Reason = why;
        return plan;
    };
    if (!enabled) {
        return clear("feedback textures off for this title");
    }
    // O mais recente escreveu os bytes que a textura lê.
    const bool useTarget = target.Present && (!display.Present || target.WriteSerial >= display.WriteSerial);
    const PicaFeedbackProducer& producer = useTarget ? target : display;
    if (producer.WriteSerial == 0U) {
        return clear("producer never written");
    }
    if (texture.NativeType != 0U) {
        return clear("texture is not 2D");
    }
    if (texture.MaxMipLevel != 0U) {
        return clear("texture is mipmapped");
    }
    // Só RGBA8 nos dois lados: as imagens da GPU são RGBA8, e outro formato no
    // mesmo endereço é reinterpretação (ou um asset que o guest carregou ali).
    if (texture.NativeFormat != 0U || producer.ColorFormat != 0U) {
        return clear("format");
    }
    if (!producer.TextureLayout) {
        return clear("layout");
    }
    // Mesma largura = mesma linha de tiles; o produtor cobre linhas de tiles
    // inteiras do começo da textura (a textura pode ser mais alta).
    if (producer.NativeWidth == 0U || producer.NativeHeight == 0U || producer.NativeWidth != texture.Width ||
        producer.NativeHeight > texture.Height || producer.NativeWidth % 8U != 0U ||
        producer.NativeHeight % 8U != 0U) {
        return clear("extent");
    }
    if (producer.AllocWidth == 0U || producer.AllocHeight == 0U || producer.ContentWidth == 0U ||
        producer.ContentHeight == 0U || producer.ContentWidth > producer.AllocWidth ||
        producer.ContentHeight > producer.AllocHeight) {
        return clear("image");
    }
    plan.Source = useTarget ? PicaFeedbackSource::Target : PicaFeedbackSource::Display;
    plan.WriteSerial = producer.WriteSerial;
    plan.ImageWidth = producer.AllocWidth;
    plan.ImageHeight = static_cast<uint32_t>(
        (static_cast<uint64_t>(texture.Height) * producer.AllocHeight + producer.NativeHeight / 2U) /
        producer.NativeHeight);
    plan.SourceWidth = producer.ContentWidth;
    plan.SourceHeight = producer.ContentHeight;
    plan.DestWidth = producer.AllocWidth;
    plan.DestHeight = producer.AllocHeight;
    plan.FlipY = viewportFlipY;
    plan.Linear = producer.ContentWidth != producer.AllocWidth || producer.ContentHeight != producer.AllocHeight;
    plan.CoversImage = plan.DestHeight == plan.ImageHeight;
    return plan;
}

} // namespace Fast::Deko3d
