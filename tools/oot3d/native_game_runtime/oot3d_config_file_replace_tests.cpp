// Testes de host para regravar um arquivo salvo no Switch.
//
// O FS do Horizon (cartão SD do Switch) recusa renomear por cima de um arquivo
// existente com EEXIST, onde POSIX e Windows substituem. Salvar uma opção pela
// segunda vez falhava no console com "cannot replace TopScreen UI config: File
// exists". Este binário define o próprio rename(), que o std::filesystem::rename
// da libstdc++ chama, para que os gravadores abaixo rodem no PC com a semântica
// do Horizon: no modo Horizon a troca recusa um destino existente (renameat2
// RENAME_NOREPLACE: EEXIST, atômico), e uma chamada escolhida pode falhar.
#include "oot3d_native_control_config.h"
#include "oot3d_top_screen_config.h"

#include "fast/renderer/cache_file.h"
#include "ship/config/ConfigPersistence.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1U << 0U)
#endif

namespace {
bool gHorizon = false;   // rename recusa um destino existente
int gRenameCalls = 0;    // renames desde o último Reset
int gFailRenameCall = 0; // essa chamada falha com EIO (0: nenhuma)
} // namespace

// Substitui o rename da libc neste processo (libstdc++ inclusive).
extern "C" __attribute__((visibility("default"))) int rename(const char *from,
                                                             const char *to) __THROW {
  ++gRenameCalls;
  if (gRenameCalls == gFailRenameCall) {
    errno = EIO;
    return -1;
  }
  const unsigned flags = gHorizon ? RENAME_NOREPLACE : 0U;
  return static_cast<int>(syscall(SYS_renameat2, AT_FDCWD, from, AT_FDCWD, to, flags));
}

namespace {
namespace fs = std::filesystem;
using namespace Oot3dNativeGame;

int gFailures = 0;

void Require(bool condition, const char *what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++gFailures;
  }
}

void Reset(bool horizon, int failCall = 0) {
  gHorizon = horizon;
  gRenameCalls = 0;
  gFailRenameCall = failCall;
}

void Write(const fs::path &path, const std::string &text) {
  std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}

std::string Read(const fs::path &path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

// O arquivo antigo enquanto o novo entra no lugar: "<arquivo>.bak".
fs::path Bak(const fs::path &path) {
  fs::path backup = path;
  backup += ".bak";
  return backup;
}

fs::path Fresh(const fs::path &root, const char *name) {
  fs::remove_all(root / name);
  fs::create_directories(root / name);
  return root / name;
}

// O interposer está valendo: o rename único dos gravadores falha como no console.
void TestHorizonRenameRefusesExistingFile(const fs::path &root) {
  const auto dir = Fresh(root, "selfcheck");
  Write(dir / "a.json", "old");
  Write(dir / "a.json.tmp", "new");
  Reset(true);
  std::error_code error;
  fs::rename(dir / "a.json.tmp", dir / "a.json", error);
  Require(gRenameCalls == 1, "std::filesystem::rename passa pelo rename interposto");
  Require(error == std::errc::file_exists, "modo Horizon: rename sobre um arquivo dá EEXIST");
  Require(error.message() == "File exists", "a mensagem do console: File exists");
  Require(Read(dir / "a.json") == "old", "o rename recusado mantém o destino");
  Reset(false);
  fs::rename(dir / "a.json.tmp", dir / "a.json", error);
  Require(!error && Read(dir / "a.json") == "new", "modo POSIX: rename substitui");
}

// O relato: as opções do TopScreen salvas uma vez e de novo por cima do arquivo.
void TestTopScreenConfig(const fs::path &root) {
  const auto dir = Fresh(root, "topscreen");
  const auto path = dir / "topscreen_ui.json";
  Reset(true);
  TopScreenUiConfig config;
  std::string error;
  Require(SaveTopScreenUiConfig(path, config, &error), "TopScreen: primeiro save");
  config.FreeCameraEnabled = true;
  config.CameraZoomPercent = 110U;
  error.clear();
  Require(SaveTopScreenUiConfig(path, config, &error), "TopScreen: save por cima do arquivo existente (o relato)");
  if (!error.empty()) std::fprintf(stderr, "  erro do TopScreen: %s\n", error.c_str());
  Require(error.empty(), "TopScreen: sem 'cannot replace TopScreen UI config: File exists'");
  TopScreenUiConfig loaded;
  Require(LoadTopScreenUiConfig(path, &loaded, &error) && loaded == config, "TopScreen: o segundo save é lido de volta");
  Require(!fs::exists(dir / "topscreen_ui.json.tmp") && !fs::exists(Bak(path)), "TopScreen: sem sobras");

  fs::rename(path, Bak(path));
  Write(dir / "topscreen_ui.json.tmp", "{");
  TopScreenUiConfig recovered;
  Require(LoadTopScreenUiConfig(path, &recovered, &error) && recovered == config,
          "TopScreen: o load depois de um save interrompido lê o último arquivo completo");
  Require(fs::exists(path) && !fs::exists(Bak(path)), "TopScreen: o backup volta para o lugar");
  Reset(false);
}

void TestControlConfig(const fs::path &root) {
  const auto dir = Fresh(root, "controls");
  const auto path = dir / "oot3d_controls.json";
  Reset(true);
  auto config = NativeControlPreset(NativeControlProfile::Controller);
  std::string error;
  Require(SaveNativeControlConfig(path, config, &error), "controle: primeiro save");
  config.NativeAimInvertY = !config.NativeAimInvertY;
  Require(SaveNativeControlConfig(path, config, &error), "controle: save por cima do arquivo existente");
  NativeControlConfig loaded;
  Require(LoadNativeControlConfig(path, &loaded, &error) && loaded == config, "controle: o segundo save é lido de volta");
  Require(!fs::exists(Bak(path)), "controle: sem backup sobrando");

  fs::rename(path, Bak(path));
  NativeControlConfig recovered;
  Require(LoadNativeControlConfig(path, &recovered, &error) && recovered == config,
          "controle: o load depois de um save interrompido lê o último arquivo completo");
  Reset(false);
}

// Ship::Config, warps de dev, idioma, preferências de savestate, ambientes.
void TestShipConfigWriter(const fs::path &root) {
  const auto dir = Fresh(root, "ship");
  const auto path = dir / "triaevum.json";
  Reset(true);
  Require(Ship::WriteConfigFileAtomically(path, "{\"a\":1}").Success, "ship: primeira gravação");
  const auto second = Ship::WriteConfigFileAtomically(path, "{\"a\":2}");
  Require(second.Success && second.Error.empty(), "ship: gravação por cima do arquivo existente");
  Require(Read(path) == "{\"a\":2}" && !fs::exists(Bak(path)), "ship: conteúdo novo, sem backup");
  for (const auto &entry : fs::directory_iterator(dir))
    Require(entry.path().filename() == "triaevum.json", "ship: sem temporário sobrando");
  Reset(false);
}

void TestCacheFileWriter(const fs::path &root) {
  const auto dir = Fresh(root, "cache");
  const auto path = dir / "entry.bin";
  const std::vector<uint8_t> first{1, 2}, second{3, 4, 5};
  Reset(true);
  std::string error;
  Require(Fast::Renderer::WriteCacheFileAtomically(path, {}, first, &error), "cache: primeira gravação");
  Require(Fast::Renderer::WriteCacheFileAtomically(path, {}, second, &error), "cache: gravação por cima da entrada existente");
  Require(Read(path) == std::string("\x03\x04\x05") && !fs::exists(Bak(path)), "cache: entrada nova, sem backup");
  Reset(false);
}

} // namespace

int main() {
  const auto root = fs::temp_directory_path() / ("oot3d_config_file_replace_tests." + std::to_string(getpid()));
  fs::create_directories(root);
  TestHorizonRenameRefusesExistingFile(root);
  TestTopScreenConfig(root);
  TestControlConfig(root);
  TestShipConfigWriter(root);
  TestCacheFileWriter(root);
  fs::remove_all(root);
  if (gFailures != 0) {
    std::fprintf(stderr, "%d falha(s)\n", gFailures);
    return EXIT_FAILURE;
  }
  std::puts("oot3d_config_file_replace_tests: ok");
  return EXIT_SUCCESS;
}
