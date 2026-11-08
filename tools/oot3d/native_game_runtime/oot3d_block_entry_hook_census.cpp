// oot3d_block_entry_hook_census: the union of every block-entry hook list
// (Oot3dNativeAllBlockEntryHookPcs, oot3d_native_block_entry_hooks.h), the
// gate list of the whole-AOT generator's --sparse-block-entries.
//
//   oot3d_block_entry_hook_census            print whole_aot_hook_gates.json
//   oot3d_block_entry_hook_census --check F  exit 1, naming the differing PCs,
//                                            when F is not that document
//
// tools/oot3d/native_a32_runtime/whole_aot_hook_gates.json is its output;
// tests/switch/run.sh runs --check on it.

#include "oot3d_native_block_entry_hooks.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

std::string Hex(uint32_t pc) {
  char text[16];
  std::snprintf(text, sizeof(text), "0x%08X", pc);
  return text;
}

nlohmann::ordered_json Census() {
  std::map<uint32_t, std::vector<std::string>> sources;
  for (const auto &hook : Oot3dNativeGame::Oot3dNativeBlockEntryHookSources(
           Oot3dNativeGame::Oot3dAllBlockEntryHookOptions())) {
    auto &names = sources[hook.Pc];
    const std::string name(hook.Source);
    if (std::find(names.begin(), names.end(), name) == names.end())
      names.push_back(name);
  }
  nlohmann::ordered_json document;
  document["format"] = "oot3d_whole_aot_hook_gates_v1";
  document["generated_by"] = "oot3d_block_entry_hook_census";
  auto &pcs = document["pcs"] = nlohmann::ordered_json::array();
  auto &bySource = document["sources"] = nlohmann::ordered_json::object();
  for (const auto &[pc, names] : sources) {
    pcs.push_back(Hex(pc));
    bySource[Hex(pc)] = names;
  }
  return document;
}

std::set<std::string> PcSet(const nlohmann::ordered_json &document) {
  std::set<std::string> pcs;
  const auto found = document.find("pcs");
  if (found == document.end() || !found->is_array()) return pcs;
  for (const auto &pc : *found)
    if (pc.is_string()) pcs.insert(pc.get<std::string>());
  return pcs;
}

int Check(const char *path, const nlohmann::ordered_json &census) {
  std::ifstream file(path);
  if (!file) {
    std::fprintf(stderr, "%s: cannot open\n", path);
    return 2;
  }
  nlohmann::ordered_json committed;
  try {
    committed = nlohmann::ordered_json::parse(file);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s: %s\n", path, error.what());
    return 2;
  }
  if (committed == census) {
    std::printf("%s: up to date (%zu hook gates)\n", path, census["pcs"].size());
    return 0;
  }
  const auto expected = PcSet(census);
  const auto actual = PcSet(committed);
  for (const auto &pc : expected)
    if (!actual.contains(pc))
      std::fprintf(stderr, "%s: missing hook gate %s\n", path, pc.c_str());
  for (const auto &pc : actual)
    if (!expected.contains(pc))
      std::fprintf(stderr, "%s: hook gate %s is no longer a hook\n", path, pc.c_str());
  std::fprintf(stderr,
               "%s is stale: regenerate it with oot3d_block_entry_hook_census "
               "(and the whole-AOT code with --sparse-block-entries)\n",
               path);
  return 1;
}

} // namespace

int main(int argc, char **argv) {
  const auto census = Census();
  if (argc == 3 && std::string(argv[1]) == "--check") return Check(argv[2], census);
  if (argc != 1) {
    std::fprintf(stderr, "usage: %s [--check whole_aot_hook_gates.json]\n", argv[0]);
    return 2;
  }
  std::cout << census.dump(2) << '\n';
  return 0;
}
