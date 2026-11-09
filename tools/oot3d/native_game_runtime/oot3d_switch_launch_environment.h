#pragma once

// How the game was started, recorded before anything else (boot-status.txt's
// "status=starting" and the game log's "launch:" line): a launch that ends
// before the game window opens then leaves the stage it reached and the
// loader that started it. Header-only; the Switch reads the values in
// oot3d_native_game.cpp. Tested in oot3d_switch_launch_environment_tests.

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace Oot3dNativeGame {

struct SwitchLaunchEnvironment {
  std::string Loader;  // the loader's notice text (nx-hbloader's name and version), first line
  int AppletType = -1;
  std::uint64_t ProgramId = 0U;
  std::uint64_t HeapBytes = 0U;  // the loader's heap override, 0 without one
  std::uint64_t UsedMemory = 0U;
  std::uint64_t TotalMemory = 0U;
  std::vector<std::string> Arguments;
};

inline std::string FirstLine(std::string_view text) {
  const auto end = text.find_first_of("\r\n");
  return std::string(text.substr(0, end == std::string_view::npos ? text.size() : end));
}

// One key=value per line, the format of boot-status.txt.
inline std::string FormatSwitchLaunchEnvironment(const SwitchLaunchEnvironment &environment) {
  char numbers[160];
  std::snprintf(numbers, sizeof(numbers),
                "applet_type=%d\nprogram_id=0x%016llX\nheap_mib=%llu\nmemory_mib=%llu/%llu\nargc=%zu\n",
                environment.AppletType, static_cast<unsigned long long>(environment.ProgramId),
                static_cast<unsigned long long>(environment.HeapBytes >> 20U),
                static_cast<unsigned long long>(environment.UsedMemory >> 20U),
                static_cast<unsigned long long>(environment.TotalMemory >> 20U), environment.Arguments.size());
  std::string text = "loader=" + (environment.Loader.empty() ? std::string("-") : environment.Loader) + '\n' + numbers;
  for (std::size_t index = 0; index < environment.Arguments.size(); ++index)
    text += "argv" + std::to_string(index) + '=' + environment.Arguments[index] + '\n';
  return text;
}

// The same values on one line, for the game log.
inline std::string FormatSwitchLaunchLine(const SwitchLaunchEnvironment &environment) {
  std::string line = "launch:";
  const std::string text = FormatSwitchLaunchEnvironment(environment);
  std::size_t start = 0;
  while (start < text.size()) {
    const auto end = text.find('\n', start);
    line += ' ' + text.substr(start, end - start);
    start = end + 1;
  }
  return line;
}

inline std::string_view WithoutQuotes(std::string_view text) {
  if (text.size() >= 2 && text.front() == '"' && text.back() == '"') return text.substr(1, text.size() - 2);
  return text;
}

// Arguments after the NRO's path that are empty or repeat that path (quoted or
// not) carry nothing: dropped, so the default launch still applies.
inline std::vector<std::string> DropRedundantLaunchArguments(const std::vector<std::string> &arguments) {
  if (arguments.empty()) return arguments;
  std::vector<std::string> kept{arguments.front()};
  const std::string_view path = WithoutQuotes(arguments.front());
  for (std::size_t index = 1; index < arguments.size(); ++index) {
    const std::string_view argument = WithoutQuotes(arguments[index]);
    if (!argument.empty() && argument != path) kept.push_back(arguments[index]);
  }
  return kept;
}

} // namespace Oot3dNativeGame
