// Unmirrored Master Quest (oot3d_master_quest_mirror.h): the read table, the
// lookup the hook uses, the mirror decision and the config file.
#include "oot3d_master_quest_mirror.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace {

using namespace Oot3dNativeGame;

void Require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

void TestTable() {
  for (size_t i = 0; i < kMasterQuestMirrorReads.size(); ++i) {
    const auto &read = kMasterQuestMirrorReads[i];
    Require(read.HookPc % 4U == 0U && read.HookPc >= 0x00100000U && read.HookPc < 0x0055B000U,
            "an ARM instruction in code.bin");
    Require(read.Register <= 2U, "the reads load r0, r1 or r2");
    Require(i == 0U || kMasterQuestMirrorReads[i - 1U].HookPc < read.HookPc, "sorted, unique");
    Require(FindMasterQuestMirrorRead(read.HookPc) == &read, "found by its hook");
    Require(FindMasterQuestMirrorRead(read.HookPc - 4U) == nullptr, "not by the read itself");
  }
  Require(FindMasterQuestMirrorRead(0x00352DCCU + 4U) == nullptr, "Health_ChangeBy's doubling stays Master Quest");
  Require(FindMasterQuestMirrorRead(0U) == nullptr && FindMasterQuestMirrorRead(0xFFFFFFFCU) == nullptr,
          "outside the table");
}

void TestDecision() {
  Require(!MasterQuestUnmirrored().load() && !MasterQuestUnmirrorApplied().load(), "mirrored by default");
  Require(MasterQuestMirrorShown(1U) && !MasterQuestMirrorShown(0U), "the game's flag");
  MasterQuestUnmirrored().store(true);
  Require(MasterQuestMirrorShown(1U), "the setting waits for the game's read");
  ApplyMasterQuestUnmirrored();
  Require(!MasterQuestMirrorShown(1U) && !MasterQuestMirrorShown(0U), "unmirrored");
  MasterQuestUnmirrored().store(false);
  Require(!MasterQuestMirrorShown(1U), "still unmirrored until the next read");
  ApplyMasterQuestUnmirrored();
  Require(MasterQuestMirrorShown(1U), "mirrored again");
  Require(FindMasterQuestMirrorRead(kMasterQuestMirrorApplyPc) == nullptr, "the apply block is not a mirror read");
}

void TestConfig() {
  const auto config = std::filesystem::temp_directory_path() / "oot3d_master_quest_mirror_tests";
  std::error_code ignored;
  std::filesystem::remove_all(config, ignored);
  Require(!ReadMasterQuestUnmirrored(config), "no file: mirrored");
  Require(SaveMasterQuestUnmirrored(config, true) && ReadMasterQuestUnmirrored(config), "on writes the file");
  Require(SaveMasterQuestUnmirrored(config, false) && !ReadMasterQuestUnmirrored(config), "off removes it");
  std::filesystem::remove_all(config, ignored);
}

} // namespace

int main() {
  TestTable();
  TestDecision();
  TestConfig();
  std::puts("oot3d_master_quest_mirror_tests: ok");
  return 0;
}
