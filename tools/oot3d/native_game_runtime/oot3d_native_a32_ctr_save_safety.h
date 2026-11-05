#pragma once

// Saves à prova de perda no backend de pasta (PC e NRO do Switch, que usam o
// mesmo código sobre o devoptab da libnx). Decisões e alternativas rejeitadas:
// docs/pesquisas/2026-11-05-saves-seguros.md.
//
// - Escrita atômica: a primeira escrita de uma sessão copia o arquivo para
//   <nome>.triaevum-tmp e toda E/S vai para essa cópia; Flush, Close e
//   ControlArchive confirmam (tmp -> <nome>.triaevum-new -> <nome>). O rename
//   do Horizon não sobrescreve (EEXIST), então a troca final é remove +
//   rename; a recuperação (primeiro OpenArchive da execução) termina uma
//   troca interrompida e tira de dentro do save o temporário que não chegou a
//   ser confirmado (vai para <save>-backups/incompleto-*, nunca é apagado).
// - Backup rotativo: antes da primeira escrita (ou do DeleteFile) de cada
//   arquivo numa execução, a versão anterior vai para
//   <save>-backups/<AAAAMMDD-HHMMSS>/; ficam as últimas kCtrSaveBackupGenerations
//   gerações, sem apagar uma geração que tenha a única cópia de um arquivo.
// - Formatação sem perda: FormatSaveData move a pasta para
//   <save>-backups/format-<AAAAMMDD-HHMMSS>/ antes de recriar.

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace Oot3dNativeGame {

inline constexpr std::string_view kCtrSaveJournalTemporarySuffix = ".triaevum-tmp";
inline constexpr std::string_view kCtrSaveJournalCommittedSuffix = ".triaevum-new";
inline constexpr size_t kCtrSaveBackupGenerations = 20U;

// Layout do Switch. O OoT3D fica EXATAMENTE onde sempre esteve (o argv padrão
// de oot3d_native_game.cpp usa o mesmo literal; guarda em test_bug_guards.py).
inline constexpr std::string_view kOot3dSwitchSaveDataDirectory = "sdmc:/switch/oot3dre/savedata";
inline constexpr std::string_view kTriAevumSwitchTitlesRoot = "sdmc:/switch/triaevum";

// sdmc:/switch/oot3dre/savedata para o OoT3D; sdmc:/switch/triaevum/<id de 16
// dígitos hex>/savedata para qualquer outro título.
std::filesystem::path CtrSwitchSaveDataDirectory(bool oot3dTitle, std::optional<uint64_t> titleId);
// A pasta pedida, exceto quando um título que não é o OoT3D cairia dentro da
// pasta de save do OoT3D (argv padrão do NRO): aí vai para a pasta do título.
std::filesystem::path ResolveCtrTitleSaveDataDirectory(const std::filesystem::path& requested,
                                                       bool oot3dTitle,
                                                       std::optional<uint64_t> titleId);

// Pasta sem separador final, normalizada só lexicalmente (funciona com
// "sdmc:/...", que o std::filesystem do devkitA64 trata como relativo).
std::filesystem::path CtrLexicalDirectory(const std::filesystem::path& directory);
// <pasta>.format, ao lado da pasta (nunca dentro dela, mesmo com '/' final).
std::filesystem::path CtrSaveFormatPath(const std::filesystem::path& saveDirectory);
// <pai>/<nome da pasta>-backups (savedata -> savedata-backups).
std::filesystem::path CtrSaveBackupRoot(const std::filesystem::path& saveDirectory);
// relative (já decodificado, sem '..' nem raiz) fica dentro de root, por
// comparação lexical; nenhum componente intermediário pode ser symlink (no
// PC um subdiretório-symlink apontando para fora é recusado). Sem
// absolute/canonical: com raiz "sdmc:/..." eles concatenam o cwd no Switch.
bool CtrArchivePathContained(const std::filesystem::path& root, const std::filesystem::path& relative);
// candidate (absoluto ou não) está dentro de root, lexicalmente.
bool CtrPathWithin(const std::filesystem::path& root, const std::filesystem::path& candidate);
// Formato sintetizado de save legado (pasta com arquivos e sem .format):
// {tamanho, dirs, arquivos, duplicado}. nullopt se a pasta não tem arquivo.
std::optional<std::array<uint32_t, 4>> CtrLegacySaveFormatInfo(const std::filesystem::path& saveDirectory);
// Apaga as gerações mais antigas além de keep, sem apagar uma geração que
// tenha a única cópia de algum arquivo. Só toca pastas AAAAMMDD-HHMMSS[-n].
void PruneCtrSaveBackups(const std::filesystem::path& backupRoot, size_t keep);
// Troca destination por source. Tenta rename; se o destino existe e o rename
// não sobrescreve (Horizon: EEXIST), remove e renomeia de novo.
bool ReplaceCtrSaveFile(const std::filesystem::path& source, const std::filesystem::path& destination);
// Testes no PC: simula o rename do Horizon (destino existente -> EEXIST).
void SetCtrSaveRenameFailsOnExistingForTesting(bool enabled) noexcept;

// Estado de uma execução sobre uma pasta de save (sobrevive ao restore de
// savestate: o host repassa o mesmo objeto).
class CtrSaveJournal {
  public:
    explicit CtrSaveJournal(std::filesystem::path saveDirectory);

    const std::filesystem::path& Directory() const noexcept { return mDirectory; }
    // file é arquivo do save (e não de extdata/RomFS)?
    bool Covers(const std::filesystem::path& file) const;
    // Uma vez por execução: termina trocas interrompidas e põe de lado os
    // temporários não confirmados. Chamado no OpenArchive(SaveData).
    void RecoverOnce();
    // Onde ler/escrever agora: o temporário se há escrita pendente.
    std::filesystem::path ActivePath(const std::filesystem::path& file) const;
    // Antes de escrever/redimensionar: backup (1x por execução) e cópia para
    // o temporário (1x por transação).
    bool BeginWrite(const std::filesystem::path& file);
    // Flush/Close: confirma a transação do arquivo (nada a fazer se não há).
    bool Commit(const std::filesystem::path& file);
    // ControlArchive(commit): confirma todas.
    bool CommitAll();
    // DeleteFile: guarda a versão atual nos backups e descarta a transação.
    bool PrepareDelete(const std::filesystem::path& file);
    // FormatSaveData: confirma o pendente e move a pasta (se tem algo) para
    // <save>-backups/format-<data>. Falso = não formatar.
    bool MoveSaveAsideForFormat();

  private:
    std::string Key(const std::filesystem::path& file) const;
    bool Backup(const std::filesystem::path& file, bool beforeDelete);
    std::filesystem::path Generation();

    std::filesystem::path mDirectory;
    std::filesystem::path mBackupRoot;
    std::filesystem::path mGeneration;
    std::set<std::string> mPending;
    std::set<std::string> mBackedUp;
    bool mRecovered = false;
};

} // namespace Oot3dNativeGame
