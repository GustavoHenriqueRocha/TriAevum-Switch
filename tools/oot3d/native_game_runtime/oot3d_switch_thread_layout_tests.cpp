// Política de threads do Switch (oot3d_switch_thread_layout.h): papel ->
// prioridade/núcleos por layout, leitura de config/thread_layout.
#include "oot3d_switch_thread_layout.h"

#include <cstdlib>
#include <iostream>

namespace {

using namespace Oot3dNativeGame;

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "oot3d_switch_thread_layout_tests: " << message << '\n';
        std::exit(1);
    }
}

constexpr uint32_t kAllCores = 0x7U;

SwitchThreadPlacement Place(SwitchThreadLayout layout, SwitchThreadRole role,
                            uint32_t mask = kAllCores) {
    return ResolveSwitchThreadPlacement(layout, role, mask);
}

void TestParse() {
    auto config = ParseSwitchThreadLayoutConfig("");
    Require(config.Valid && config.Layout == SwitchThreadLayout::Fixed &&
                config.PacerSpinMicroseconds == 0,
            "empty file must be fixed with no pacer spin");
    config = ParseSwitchThreadLayoutConfig("float\n");
    Require(config.Valid && config.Layout == SwitchThreadLayout::Float, "float");
    config = ParseSwitchThreadLayoutConfig("legacy");
    Require(config.Valid && config.Layout == SwitchThreadLayout::Legacy &&
                config.PacerSpinMicroseconds == 500,
            "legacy must restore the 500 us pacer spin");
    config = ParseSwitchThreadLayoutConfig("float pacer_spin_us=200\r\n");
    Require(config.Valid && config.Layout == SwitchThreadLayout::Float &&
                config.PacerSpinMicroseconds == 200,
            "explicit spin after the mode");
    config = ParseSwitchThreadLayoutConfig("legacy,pacer_spin_us=0");
    Require(config.Valid && config.PacerSpinMicroseconds == 0,
            "explicit spin overrides the legacy default");
    config = ParseSwitchThreadLayoutConfig("pacer_spin_us=50");
    Require(config.Valid && config.Layout == SwitchThreadLayout::Fixed &&
                config.PacerSpinMicroseconds == 50,
            "spin alone keeps fixed");
    config = ParseSwitchThreadLayoutConfig("turbo");
    Require(!config.Valid && config.Layout == SwitchThreadLayout::Fixed,
            "unknown mode falls back to fixed, marked invalid");
    config = ParseSwitchThreadLayoutConfig("fixed pacer_spin_us=abc");
    Require(!config.Valid && config.PacerSpinMicroseconds == 0,
            "bad spin value is ignored");
    config = ParseSwitchThreadLayoutConfig("fixed pacer_spin_us=99999");
    Require(!config.Valid && config.PacerSpinMicroseconds == 0,
            "spin above 4 ms is refused");
}

// O que nunca pode acontecer em layout nenhum: thread de serviço no núcleo
// do jogo ou com prioridade capaz de interromper render/planos/áudio;
// render/planos acima do jogo; áudio abaixo da render thread.
void TestInvariants() {
    for (const auto layout : {SwitchThreadLayout::Fixed, SwitchThreadLayout::Float,
                              SwitchThreadLayout::Legacy}) {
        const auto game = Place(layout, SwitchThreadRole::Game);
        const auto render = Place(layout, SwitchThreadRole::Render);
        const auto plan = Place(layout, SwitchThreadRole::PicaPlan);
        const auto audio = Place(layout, SwitchThreadRole::HostAudio);
        const auto sdlAudio = Place(layout, SwitchThreadRole::SdlAudio);
        const auto service = Place(layout, SwitchThreadRole::Service);
        Require(game.CoreMask == 0x1U && game.IdealCore == 0, "game owns core 0");
        Require((render.CoreMask & 0x1U) == 0U && (plan.CoreMask & 0x1U) == 0U &&
                    (audio.CoreMask & 0x1U) == 0U && (sdlAudio.CoreMask & 0x1U) == 0U &&
                    (service.CoreMask & 0x1U) == 0U,
                "no other thread may run on the game's core");
        Require(render.Priority > game.Priority && plan.Priority > render.Priority,
                "render below the game, plan worker below render");
        Require(audio.Priority < render.Priority && sdlAudio.Priority < render.Priority,
                "audio must preempt the render thread");
        Require(render.CoreMask != 0U &&
                    (render.CoreMask & (1U << render.IdealCore)) != 0U,
                "render ideal core inside its mask");
        if (layout != SwitchThreadLayout::Legacy) {
            Require(service.Priority == kSwitchServicePriority &&
                        service.Priority > plan.Priority,
                    "service threads below every thread that matters");
            Require(service.CoreMask == 0x6U, "service threads float on cores 1|2");
            Require(sdlAudio.CoreMask == 0x2U && sdlAudio.IdealCore == 1,
                    "SDL audio joins the host audio on core 1");
        }
    }
}

void TestLayouts() {
    auto render = Place(SwitchThreadLayout::Fixed, SwitchThreadRole::Render);
    Require(render.CoreMask == 0x4U && render.IdealCore == 2 &&
                render.Priority == 0x2D,
            "fixed: render pinned to core 2 at 0x2D");
    auto plan = Place(SwitchThreadLayout::Fixed, SwitchThreadRole::PicaPlan);
    Require(plan.CoreMask == 0x2U && plan.Priority == 0x2E, "fixed: plan on core 1");
    render = Place(SwitchThreadLayout::Float, SwitchThreadRole::Render);
    Require(render.CoreMask == 0x6U && render.IdealCore == 2,
            "float: render may run on cores 1|2, ideal 2");
    plan = Place(SwitchThreadLayout::Float, SwitchThreadRole::PicaPlan);
    Require(plan.CoreMask == 0x6U && plan.IdealCore == 1,
            "float: plan worker on cores 1|2, ideal 1");
    const auto audio = Place(SwitchThreadLayout::Float, SwitchThreadRole::HostAudio);
    Require(audio.CoreMask == 0x2U, "audio stays pinned in float");
    // Legacy = the previous code exactly.
    const auto service = Place(SwitchThreadLayout::Legacy, SwitchThreadRole::Service);
    Require(service.Priority == 0x2C && service.IdealCore == -2 && service.CoreMask == 0U,
            "legacy: new pthreads at the game's priority, alternating cores");
    const auto sdl = Place(SwitchThreadLayout::Legacy, SwitchThreadRole::SdlAudio);
    Require(sdl.Priority == 0x2B && sdl.IdealCore == -1 && sdl.CoreMask == 0U,
            "legacy: SDL audio keeps its creation core");
}

void TestProcessMask() {
    // Process without core 2: fixed leaves the render thread alone; float
    // keeps core 1.
    auto render = Place(SwitchThreadLayout::Fixed, SwitchThreadRole::Render, 0x3U);
    Require(render.CoreMask == 0U && render.IdealCore == -1,
            "fixed render without core 2: affinity left alone");
    render = Place(SwitchThreadLayout::Float, SwitchThreadRole::Render, 0x3U);
    Require(render.CoreMask == 0x2U && render.IdealCore == 1,
            "float render without core 2: core 1 is ideal");
    const auto game = Place(SwitchThreadLayout::Fixed, SwitchThreadRole::Game, 0x6U);
    Require(game.CoreMask == 0U && game.IdealCore == -1,
            "game without core 0: affinity left alone");
}

} // namespace

int main() {
    TestParse();
    TestInvariants();
    TestLayouts();
    TestProcessMask();
    std::cout << "oot3d_switch_thread_layout_tests: ok\n";
    return 0;
}
