#pragma once

// Perfil de pico do código guest (config/guest_spike_ms).
//
// tour19 (pgo5n, 2026-10-23): 79% dos 428 refreshes perdidos tiveram a
// thread do jogo como culpada (stage=guest em logs/spikes.txt); fora dos
// carregamentos foram 1-10 por janela, e o Water Temple teve 38 numa janela
// (guest_ms 20-26 contra 8-11 de média). spikes.txt diz QUANDO; este perfil
// diz QUAL código do jogo pesou naqueles quadros.
//
// Em cima do GuestBlockProfiler (oot3d_native_guest_profile.h, modo "todos
// os blocos", relógio cntpct_el0): cada intervalo atribuído a um bloco
// também entra no acumulador do quadro (Add, caminho quente: um uint32 por
// bloco do texto e uma lista de blocos tocados, ambos alocados uma vez). No
// fim do quadro do host (EndFrame):
//  - quadro com tempo guest > limite: agrega os blocos tocados por função
//    (mapa config/guest_functions.txt; sem ele, por bloco), guarda as N
//    mais caras num anel fixo e soma todas no resumo (tabela hash fixa);
//  - sempre: zera só os blocos tocados.
// Formatação e escrita ficam fora do quadro (DrainLines/FormatSummary, na
// janela do perf.txt): logs/guest_spikes.txt (um bloco por pico) e
// logs/guest_spikes_summary.txt (top 30 por tempo total nos quadros com
// pico, reescrito a cada janela: vale "ao fim" mesmo se o jogo for fechado
// pelo HOME).
//
// Custo: o modo todos os blocos chama o callback em cada entrada de bloco
// (o jogo fica mais lento enquanto ligado; os tempos incluem essa
// sobrecarga, ~constante por entrada: as "entries" de cada pico permitem
// descontá-la). Por isso fica atrás da chave, desligado por padrão.
//
// config/guest_spike_ms: presente liga. Conteúdo opcional "limite [corte]":
// limite em ms (padrão 14) e corte de carregamento em ms (padrão 50; quadros
// acima dele vão para o log com load=1 mas não para o resumo).
//
// Lógica pura (sem arquivo, sem relógio): oot3d_guest_spike_profile_tests.cpp.

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Oot3dNativeGame {

inline constexpr uint32_t kGuestSpikeMaxTop = 16U;

struct GuestSpikeConfig {
  double ThresholdMs = 14.0;
  double LoadMs = 50.0;
  uint32_t TopPerSpike = 10U;      // <= kGuestSpikeMaxTop
  uint32_t TouchedCapacity = 65536U;
  uint32_t RingCapacity = 128U;
  uint32_t SummaryCapacity = 16384U; // potência de 2
  uint32_t SummaryTop = 30U;
};

// "14", "12,5", "14 60": limite (1-200 ms) e corte de carregamento
// (> limite, até 5000). Vazio ou inválido: os padrões.
inline void ParseGuestSpikeConfig(std::string_view text,
                                  GuestSpikeConfig *config) noexcept {
  const auto number = [&](double *out) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' ||
                             text.front() == '\r' || text.front() == '\n'))
      text.remove_prefix(1U);
    double value = 0.0, divisor = 1.0;
    bool digits = false, point = false;
    while (!text.empty()) {
      const char c = text.front();
      if (c >= '0' && c <= '9') {
        digits = true;
        if (point) {
          divisor *= 10.0;
          value += (c - '0') / divisor;
        } else {
          value = value * 10.0 + (c - '0');
        }
      } else if ((c == '.' || c == ',') && !point) {
        point = true;
      } else {
        break;
      }
      text.remove_prefix(1U);
    }
    if (digits)
      *out = value;
    return digits;
  };
  double threshold = 0.0, load = 0.0;
  if (number(&threshold) && threshold >= 1.0 && threshold <= 200.0)
    config->ThresholdMs = threshold;
  if (number(&load) && load > config->ThresholdMs && load <= 5000.0)
    config->LoadMs = load;
  if (config->LoadMs <= config->ThresholdMs)
    config->LoadMs = config->ThresholdMs * 4.0;
}

struct GuestSpikeEntry {
  uint32_t Pc = 0U; // entrada da função (ou o bloco, sem mapa)
  uint32_t Ticks = 0U;
};

struct GuestSpikeRecord {
  uint64_t HostFrame = 0U;
  uint64_t FrameTicks = 0U;
  uint64_t Entries = 0U;  // entradas de bloco no quadro
  uint32_t Blocks = 0U;   // blocos distintos
  uint32_t Functions = 0U;
  bool Overflow = false;  // mais blocos que TouchedCapacity
  bool Load = false;      // acima do corte de carregamento
  uint32_t Count = 0U;
  std::array<GuestSpikeEntry, kGuestSpikeMaxTop> Top{};
};

struct GuestSpikeTotals {
  uint64_t Frames = 0U;
  uint64_t Spikes = 0U;
  uint64_t LoadSpikes = 0U;
  uint64_t FrameTicks = 0U;
  uint64_t SpikeTicks = 0U; // quadros com pico fora de carregamento
  uint64_t RingDropped = 0U;
  uint64_t OverflowFrames = 0U;
  uint64_t SummaryFull = 0U;
};

class GuestSpikeProfile {
public:
  // functionEntries: entradas de função em ordem crescente (pode ser vazio).
  void Configure(const GuestSpikeConfig &config, uint32_t base, uint32_t slots,
                 double ticksPerMs, std::span<const uint32_t> functionEntries) {
    mConfig = config;
    mConfig.TopPerSpike = std::clamp<uint32_t>(mConfig.TopPerSpike, 1U,
                                               kGuestSpikeMaxTop);
    uint32_t capacity = 1U;
    while (capacity < std::max<uint32_t>(mConfig.SummaryCapacity, 64U))
      capacity <<= 1U;
    mConfig.SummaryCapacity = capacity;
    mBase = base;
    mTicksPerMs = ticksPerMs > 0.0 ? ticksPerMs : 1.0;
    mThresholdTicks = static_cast<uint64_t>(mConfig.ThresholdMs * mTicksPerMs);
    mLoadTicks = static_cast<uint64_t>(mConfig.LoadMs * mTicksPerMs);
    mFrameTicks.assign(slots, 0U);
    mTouched.assign(std::max<uint32_t>(mConfig.TouchedCapacity, 1U), 0U);
    mTouchedCount = 0U;
    mScratch.assign(mTouched.size(), GuestSpikeEntry{});
    mRing.assign(std::max<uint32_t>(mConfig.RingCapacity, 1U), GuestSpikeRecord{});
    mRingHead = mRingSize = 0U;
    mSummary.assign(mConfig.SummaryCapacity, SummarySlot{});
    mSummaryUsed = 0U;
    mFunctions.assign(functionEntries.begin(), functionEntries.end());
    mFrameTotal = 0U;
    mOverflow = false;
    mTotals = {};
    mEnabled = slots != 0U;
  }

  [[nodiscard]] bool Enabled() const noexcept { return mEnabled; }
  [[nodiscard]] const GuestSpikeConfig &Config() const noexcept { return mConfig; }
  [[nodiscard]] const GuestSpikeTotals &Totals() const noexcept { return mTotals; }
  [[nodiscard]] double TicksPerMs() const noexcept { return mTicksPerMs; }

  // Caminho quente: `ticks` atribuídos ao bloco `slot` neste quadro.
  void Add(uint32_t slot, uint64_t ticks) noexcept {
    mFrameTotal += ticks;
    if (slot >= mFrameTicks.size() || ticks == 0U)
      return;
    uint32_t &frame = mFrameTicks[slot];
    if (frame == 0U) {
      if (mTouchedCount >= mTouched.size()) {
        mOverflow = true;
        return;
      }
      mTouched[mTouchedCount++] = slot;
    }
    const uint64_t sum = static_cast<uint64_t>(frame) + ticks;
    frame = sum > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(sum);
  }

  // Fim do quadro do host. true: o quadro foi um pico (registrado).
  bool EndFrame(uint64_t hostFrame, uint64_t blockEntries) {
    if (!mEnabled)
      return false;
    ++mTotals.Frames;
    mTotals.FrameTicks += mFrameTotal;
    const bool spike = mFrameTotal > mThresholdTicks;
    if (spike)
      RecordSpike(hostFrame, blockEntries);
    for (uint32_t i = 0; i < mTouchedCount; ++i)
      mFrameTicks[mTouched[i]] = 0U;
    mTouchedCount = 0U;
    mFrameTotal = 0U;
    mOverflow = false;
    return spike;
  }

  [[nodiscard]] size_t Pending() const noexcept { return mRingSize; }

  // Picos guardados -> texto (fora do quadro). nameOf(pc) -> const char*
  // ("" sem nome). Esvazia o anel.
  template <typename NameOf>
  size_t DrainLines(std::string &out, NameOf &&nameOf) {
    size_t drained = 0U;
    char line[256];
    while (mRingSize != 0U) {
      const auto &record = mRing[(mRingHead + mRing.size() - mRingSize) % mRing.size()];
      --mRingSize;
      ++drained;
      const double frameMs = static_cast<double>(record.FrameTicks) / mTicksPerMs;
      std::snprintf(line, sizeof(line),
                    "spike host_frame=%" PRIu64 " guest_ms=%.2f entries=%" PRIu64
                    " blocks=%u functions=%u load=%d overflow=%d\n",
                    record.HostFrame, frameMs, record.Entries, record.Blocks,
                    record.Functions, record.Load ? 1 : 0, record.Overflow ? 1 : 0);
      out += line;
      for (uint32_t i = 0; i < record.Count; ++i) {
        const auto &entry = record.Top[i];
        const double ms = static_cast<double>(entry.Ticks) / mTicksPerMs;
        const char *name = nameOf(entry.Pc);
        std::snprintf(line, sizeof(line), "  fn=0x%08X ms=%.3f pct=%.1f name=%s\n",
                      entry.Pc, ms, frameMs > 0.0 ? 100.0 * ms / frameMs : 0.0,
                      name != nullptr && name[0] != '\0' ? name : "-");
        out += line;
      }
    }
    return drained;
  }

  // Resumo agregado: as SummaryTop funções de maior tempo total nos quadros
  // com pico (fora de carregamento).
  template <typename NameOf>
  void FormatSummary(std::string &out, NameOf &&nameOf) const {
    std::vector<SummarySlot> rows;
    rows.reserve(mSummaryUsed);
    for (const auto &slot : mSummary)
      if (slot.Pc != 0U)
        rows.push_back(slot);
    std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
      return a.Ticks != b.Ticks ? a.Ticks > b.Ticks : a.Pc < b.Pc;
    });
    const auto ms = [&](uint64_t ticks) {
      return static_cast<double>(ticks) / mTicksPerMs;
    };
    const uint64_t normalSpikes = mTotals.Spikes - mTotals.LoadSpikes;
    char line[320];
    std::snprintf(
        line, sizeof(line),
        "# triaevum guest_spikes v1\n"
        "# limite_ms=%.2f corte_carregamento_ms=%.2f quadros=%" PRIu64
        " picos=%" PRIu64 " picos_carregamento=%" PRIu64 "\n"
        "# guest_ms_medio=%.3f guest_ms_medio_pico=%.3f anel_descartados=%" PRIu64
        " quadros_overflow=%" PRIu64 " resumo_cheio=%" PRIu64 "\n",
        mConfig.ThresholdMs, mConfig.LoadMs, mTotals.Frames, mTotals.Spikes,
        mTotals.LoadSpikes,
        mTotals.Frames != 0U ? ms(mTotals.FrameTicks) / static_cast<double>(mTotals.Frames) : 0.0,
        normalSpikes != 0U ? ms(mTotals.SpikeTicks) / static_cast<double>(normalSpikes) : 0.0,
        mTotals.RingDropped, mTotals.OverflowFrames, mTotals.SummaryFull);
    out += line;
    out += "# top por tempo total nos quadros com pico: entrada | ms_total | "
           "%tempo_pico | quadros | ms/quadro | ms_max | nome\n";
    const size_t limit = std::min<size_t>(rows.size(), mConfig.SummaryTop);
    for (size_t i = 0; i < limit; ++i) {
      const auto &row = rows[i];
      const char *name = nameOf(row.Pc);
      std::snprintf(line, sizeof(line), "0x%08X %9.2f %6.2f%% %6u %7.3f %7.3f %s\n",
                    row.Pc, ms(row.Ticks),
                    mTotals.SpikeTicks != 0U
                        ? 100.0 * static_cast<double>(row.Ticks) /
                              static_cast<double>(mTotals.SpikeTicks)
                        : 0.0,
                    row.Frames,
                    row.Frames != 0U ? ms(row.Ticks) / static_cast<double>(row.Frames) : 0.0,
                    ms(row.MaxTicks),
                    name != nullptr && name[0] != '\0' ? name : "-");
      out += line;
    }
  }

private:
  struct SummarySlot {
    uint32_t Pc = 0U;
    uint32_t Frames = 0U;
    uint64_t Ticks = 0U;
    uint32_t MaxTicks = 0U;
  };

  uint32_t KeyOf(uint32_t slot) const noexcept {
    const uint32_t pc = mBase + slot * 4U;
    if (mFunctions.empty())
      return pc;
    const auto it = std::upper_bound(mFunctions.begin(), mFunctions.end(), pc);
    return it == mFunctions.begin() ? pc : *std::prev(it);
  }

  void RecordSpike(uint64_t hostFrame, uint64_t blockEntries) {
    ++mTotals.Spikes;
    const bool load = mFrameTotal > mLoadTicks;
    if (load)
      ++mTotals.LoadSpikes;
    else
      mTotals.SpikeTicks += mFrameTotal;
    if (mOverflow)
      ++mTotals.OverflowFrames;
    // Blocos -> funções: ordena por chave e soma (scratch pré-alocado).
    for (uint32_t i = 0; i < mTouchedCount; ++i)
      mScratch[i] = {KeyOf(mTouched[i]), mFrameTicks[mTouched[i]]};
    auto *begin = mScratch.data();
    auto *end = begin + mTouchedCount;
    std::sort(begin, end, [](const auto &a, const auto &b) { return a.Pc < b.Pc; });
    auto *write = begin;
    for (auto *read = begin; read != end; ++read) {
      if (write != begin && std::prev(write)->Pc == read->Pc) {
        const uint64_t sum = static_cast<uint64_t>(std::prev(write)->Ticks) + read->Ticks;
        std::prev(write)->Ticks = sum > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(sum);
      } else {
        *write++ = *read;
      }
    }
    const size_t functions = static_cast<size_t>(write - begin);
    if (!load)
      for (size_t i = 0; i < functions; ++i)
        AddSummary(begin[i]);
    const size_t top = std::min<size_t>(functions, mConfig.TopPerSpike);
    std::partial_sort(begin, begin + top, write, [](const auto &a, const auto &b) {
      return a.Ticks != b.Ticks ? a.Ticks > b.Ticks : a.Pc < b.Pc;
    });
    if (mRingSize == mRing.size()) {
      ++mTotals.RingDropped; // o mais antigo sai
      --mRingSize;
    }
    auto &record = mRing[mRingHead];
    mRingHead = (mRingHead + 1U) % mRing.size();
    ++mRingSize;
    record.HostFrame = hostFrame;
    record.FrameTicks = mFrameTotal;
    record.Entries = blockEntries;
    record.Blocks = mTouchedCount;
    record.Functions = static_cast<uint32_t>(functions);
    record.Overflow = mOverflow;
    record.Load = load;
    record.Count = static_cast<uint32_t>(top);
    for (size_t i = 0; i < top; ++i)
      record.Top[i] = begin[i];
  }

  void AddSummary(const GuestSpikeEntry &entry) noexcept {
    const uint32_t mask = static_cast<uint32_t>(mSummary.size()) - 1U;
    uint32_t index = ((entry.Pc >> 2U) * 2654435761U) & mask;
    for (uint32_t probe = 0; probe <= mask; ++probe) {
      auto &slot = mSummary[index];
      if (slot.Pc == entry.Pc || slot.Pc == 0U) {
        if (slot.Pc == 0U) {
          if (mSummaryUsed * 4U >= mSummary.size() * 3U) {
            ++mTotals.SummaryFull;
            return;
          }
          slot.Pc = entry.Pc;
          ++mSummaryUsed;
        }
        ++slot.Frames;
        slot.Ticks += entry.Ticks;
        slot.MaxTicks = std::max(slot.MaxTicks, entry.Ticks);
        return;
      }
      index = (index + 1U) & mask;
    }
    ++mTotals.SummaryFull;
  }

  GuestSpikeConfig mConfig{};
  bool mEnabled = false;
  uint32_t mBase = 0U;
  double mTicksPerMs = 1.0;
  uint64_t mThresholdTicks = 0U;
  uint64_t mLoadTicks = 0U;
  std::vector<uint32_t> mFrameTicks;
  std::vector<uint32_t> mTouched;
  uint32_t mTouchedCount = 0U;
  std::vector<GuestSpikeEntry> mScratch;
  std::vector<GuestSpikeRecord> mRing;
  size_t mRingHead = 0U;
  size_t mRingSize = 0U;
  std::vector<SummarySlot> mSummary;
  size_t mSummaryUsed = 0U;
  std::vector<uint32_t> mFunctions;
  uint64_t mFrameTotal = 0U;
  bool mOverflow = false;
  GuestSpikeTotals mTotals{};
};

} // namespace Oot3dNativeGame
