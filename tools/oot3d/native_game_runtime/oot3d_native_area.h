#pragma once

// The game's current area (scene and room) for the console logs: every
// logs/hitches.txt line carries the area of its frame and every logs/perf.txt
// window the area it spent most frames in, so slow spots can be named.
// Read once per host frame from guest memory (four reads); header-only, tests
// in oot3d_native_perf_diagnostics_tests.cpp.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace Oot3dNativeGame {

// The current GameState (0 between states); its u8 at +0x100 is 3 for the
// PlayState (the TopScreen pause and HUD code read the same pair). PlayState
// fields: scene id s16 at +0x104, current room s8 at +0x4C30
// (OOT3D_CURRENT_ROOM_PLAY_OFFSET, decomp_support/include/oot3d/actor_spawn.h).
inline constexpr uint32_t kNativeGameStatePointer = 0x005043E0U;
inline constexpr uint32_t kNativeGameStateTypeOffset = 0x100U;
inline constexpr uint8_t kNativeGameStatePlay = 3U;
inline constexpr uint32_t kNativePlaySceneOffset = 0x104U;
inline constexpr uint32_t kNativePlayRoomOffset = 0x4C30U;

// Scene file stems by scene id, the title's scene table order (the same table
// as tools/triaevum_release/scene_lights.py SCENE_NAMES); nullptr = unnamed.
inline constexpr std::array<const char *, 108> kNativeSceneStems{
    "ydan", "ddan", "bdan", "bmori1", "hidan", "mizusin", "jyasinzou", "hakadan", "hakadan_ch",
    "ice_doukutu", "ganon", "men", "gerudoway", "ganontika", "ganon_sonogo", "ganontikasonogo",
    "takaraya", "ydan_boss", "ddan_boss", "bdan_boss", "moriboss", "fire_bs", "mizusin_boss",
    "jyasinzou_boss", "hakadan_boss", "ganon_boss", "ganon_final", "entra_day", "entra_night",
    "entra_ruins", "market_alley", "market_alley_n", "market_day", "market_night", "market_ruins",
    "shrine", "shrine_n", "shrine_r", "k_home", "k_home3", "k_home4", "k_home5", "kakariko",
    "kakariko_home3", "shop", "kokiri", "shop_golon", "zoora", "shop_drag", "shop_alley",
    "shop_night", "shop_face", "link", "kakariko_impa", "stable", "labo", "hylia_labo", "tent",
    "hut", "daiyousei_izumi", "yousei_izumi_tate", "yousei_izumi_yoko", "kakusiana", "hakaana",
    "hakaana2", "hakaana_ouke", "syatekijyou", "tokinoma", "kenjyanoma", "hairal_niwa",
    "hairal_niwa_n", "hiral_demo", "hakasitarelay", "turibori", "nakaniwa", "bowling", "souko",
    "miharigoya", "mahouya", "ganon_demo", "kinsuta", "spot00", "spot01", "spot02", "spot03",
    "spot04", "spot05", "spot06", "spot07", "spot08", "spot09", "spot10", "spot11", "spot12",
    "spot13", "spot15", "spot16", "spot17", "spot18", "spot20", "ganon_tou", nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, "spot99",
};

[[nodiscard]] inline const char *NativeSceneStem(int scene) {
  return scene >= 0 && static_cast<size_t>(scene) < kNativeSceneStems.size()
             ? kNativeSceneStems[static_cast<size_t>(scene)]
             : nullptr;
}

// -1 when the stem names no scene.
[[nodiscard]] inline int NativeSceneIdByStem(std::string_view stem) {
  for (size_t scene = 0; scene < kNativeSceneStems.size(); ++scene)
    if (kNativeSceneStems[scene] != nullptr && stem == kNativeSceneStems[scene])
      return static_cast<int>(scene);
  return -1;
}

struct NativeArea {
  int16_t Scene = -1; // -1: not in gameplay
  int8_t Room = -1;   // -1: no room
  bool operator==(const NativeArea &) const = default;
};

// Any guest memory with Read8/Read16/Read32 (oot3d::recomp::a32::MemoryBus).
template <typename Memory> [[nodiscard]] NativeArea ReadNativeArea(Memory &memory) {
  NativeArea area;
  uint32_t play = 0U;
  uint8_t type = 0U;
  uint16_t scene = 0U;
  uint8_t room = 0xFFU;
  if (!memory.Read32(kNativeGameStatePointer, &play) || play == 0U ||
      !memory.Read8(play + kNativeGameStateTypeOffset, &type) || type != kNativeGameStatePlay ||
      !memory.Read16(play + kNativePlaySceneOffset, &scene) || scene >= 0x8000U)
    return area;
  area.Scene = static_cast<int16_t>(scene);
  if (memory.Read8(play + kNativePlayRoomOffset, &room))
    area.Room = static_cast<int8_t>(room);
  return area;
}

// "scene=0x55:spot04 room=2"; "scene=0x65 room=0" without a stem; outside
// gameplay "scene=- room=-".
[[nodiscard]] inline std::string FormatNativeArea(NativeArea area) {
  char text[48];
  if (area.Scene < 0) return "scene=- room=-";
  const char *stem = NativeSceneStem(area.Scene);
  const int length = area.Room >= 0
      ? std::snprintf(text, sizeof(text), "scene=0x%02X%s%s room=%d", static_cast<unsigned>(area.Scene),
                      stem != nullptr ? ":" : "", stem != nullptr ? stem : "", static_cast<int>(area.Room))
      : std::snprintf(text, sizeof(text), "scene=0x%02X%s%s room=-", static_cast<unsigned>(area.Scene),
                      stem != nullptr ? ":" : "", stem != nullptr ? stem : "");
  return std::string(text, static_cast<size_t>(length > 0 ? length : 0));
}

// The areas of one perf.txt window, one Observe per host frame. The window's
// area is the scene with the most frames (frames outside gameplay count as
// their own "scene") and that scene's most frequent room; scene changes count
// every switch between two gameplay scenes, including one across the window
// boundary (the last scene is kept by Reset). No allocation.
class NativeAreaWindow {
public:
  struct Summary {
    NativeArea Area;
    uint32_t SceneChanges = 0U;
  };

  void Observe(NativeArea area) {
    if (area.Scene >= 0) {
      if (mLastScene >= 0 && area.Scene != mLastScene) ++mSceneChanges;
      mLastScene = area.Scene;
    }
    for (size_t index = 0; index < mCount; ++index) {
      if (mEntries[index].Area == area) {
        ++mEntries[index].Frames;
        return;
      }
    }
    if (mCount < mEntries.size()) mEntries[mCount++] = {area, 1U};
  }

  [[nodiscard]] Summary Dominant() const {
    Summary summary;
    summary.SceneChanges = mSceneChanges;
    uint32_t bestSceneFrames = 0U;
    for (size_t index = 0; index < mCount; ++index) {
      const int16_t scene = mEntries[index].Area.Scene;
      uint32_t frames = 0U;
      for (size_t other = 0; other < mCount; ++other)
        if (mEntries[other].Area.Scene == scene) frames += mEntries[other].Frames;
      if (frames > bestSceneFrames) {
        bestSceneFrames = frames;
        summary.Area.Scene = scene;
      }
    }
    uint32_t bestRoomFrames = 0U;
    for (size_t index = 0; index < mCount; ++index) {
      if (mEntries[index].Area.Scene == summary.Area.Scene && mEntries[index].Frames > bestRoomFrames) {
        bestRoomFrames = mEntries[index].Frames;
        summary.Area.Room = mEntries[index].Area.Room;
      }
    }
    return summary;
  }

  void Reset() {
    mCount = 0U;
    mSceneChanges = 0U;
  }

private:
  struct Entry {
    NativeArea Area;
    uint32_t Frames = 0U;
  };
  // Distinct areas of one window; later ones are not counted.
  std::array<Entry, 16> mEntries{};
  size_t mCount = 0U;
  uint32_t mSceneChanges = 0U;
  int16_t mLastScene = -1;
};

} // namespace Oot3dNativeGame
