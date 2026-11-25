#pragma once

// Tempo de host por SVC (config/svc_profile no Switch, TRIAEVUM_SVC_PROFILE no
// PC; desligado por padrão). NativeA32CtrHostServices::HandleSvc soma, por
// chave, quantas vezes cada SVC rodou e quanto o host levou para atendê-lo; o
// SendSyncRequest vai por sessão e comando ("0x32 SendSyncRequest:gsp::Gpu
// cmd=0x0008"). É o tempo do HLE, não a espera do guest: um WaitSynchronization
// que estaciona a thread conta só o atendimento.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace Oot3dNativeGame {

struct SvcTimeStat {
    uint64_t Count = 0;
    uint64_t Nanoseconds = 0;
};

class SvcTimeProfile final {
  public:
    void Add(const std::string& key, uint64_t nanoseconds) {
        auto& stat = mStats[key];
        ++stat.Count;
        stat.Nanoseconds += nanoseconds;
    }

    const std::map<std::string, SvcTimeStat>& Stats() const noexcept { return mStats; }

    // Tabela do mais caro (tempo total) ao mais barato; frames = quadros do
    // host desde o início, para a coluna por quadro (0 omite a coluna).
    std::string Format(uint64_t frames) const {
        std::vector<std::pair<std::string, SvcTimeStat>> rows(mStats.begin(), mStats.end());
        std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) {
            return left.second.Nanoseconds > right.second.Nanoseconds;
        });
        uint64_t total = 0;
        for (const auto& row : rows) total += row.second.Nanoseconds;
        std::string out = "# triaevum svc_profile v1\n# quadros=" + std::to_string(frames) +
                          " total_ms=" + Fixed(total / 1.0e6, 1) + "\n" +
                          "# ms_total | us/chamada | chamadas | chamadas/quadro | ms/quadro | chave\n";
        for (const auto& [key, stat] : rows) {
            const double ms = stat.Nanoseconds / 1.0e6;
            out += Fixed(ms, 1) + " | " + Fixed(stat.Nanoseconds / 1.0e3 / stat.Count, 2) + " | " +
                   std::to_string(stat.Count) + " | " +
                   (frames ? Fixed(static_cast<double>(stat.Count) / frames, 1) : "-") + " | " +
                   (frames ? Fixed(ms / frames, 3) : "-") + " | " + key + "\n";
        }
        return out;
    }

  private:
    static std::string Fixed(double value, int decimals) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.*f", decimals, value);
        return text;
    }

    std::map<std::string, SvcTimeStat> mStats;
};

} // namespace Oot3dNativeGame
