#pragma once

// Saves à prova de perda no backend de pasta (PC e NRO do Switch, que usam o
// mesmo código sobre o devoptab da libnx). Decisões e alternativas rejeitadas:
// docs/pesquisas/2026-11-05-saves-seguros.md.
//
// - Escrita atômica: a primeira escrita de uma transação copia o arquivo para
//   <nome>.triaevum-tmp e toda E/S vai para essa cópia. Regra de commit (por
//   transação e handle): a transação de um arquivo é confirmada quando o
//   ÚLTIMO handle que escreveu nela dá Flush ou fecha (FSFILE_Close ou
//   svcCloseHandle), ou no ControlArchive (commit do archive inteiro, como o
//   commit de save com journaling do 3DS). Handle só de leitura nunca
//   confirma. Write/SetSize que falha (SD cheio) marca a transação como
//   falha: ela nunca é instalada; vai para <save>-backups/incompleto-*.
//   Confirmar = tmp -> <nome>.triaevum-new -> <nome>. O rename do Horizon
//   não sobrescreve (EEXIST): o destino vira <nome>.triaevum-old, o novo
//   entra, e só então o .old sai; se o rename do novo falha, o .old volta.
//   A recuperação (primeiro acesso ao save da execução: OpenArchive,
//   OpenFile, E/S, GetFormatInfo ou restore de savestate) instala o .new,
//   devolve o .old se o arquivo sumiu e tira de dentro do save o temporário
//   que não foi confirmado (vai para incompleto-*, nunca é apagado).
// - Backup rotativo: antes da primeira escrita (ou do DeleteFile) de cada
//   arquivo numa execução, a versão anterior vai para
//   <save>-backups/<AAAAMMDD-HHMMSS>/. A cópia é gravada como
//   <nome>.triaevum-tmp, conferida (tamanho + checksum contra a origem) e só
//   então renomeada; cópia que não confere é descartada e não conta como
//   cópia. A ordem das gerações vem de <save>-backups/.triaevum-geracoes
//   (ordem de criação), não da data no nome (o relógio do console pode
//   voltar). Ficam as últimas kCtrSaveBackupGenerations, sem apagar uma
//   geração que tenha a única cópia de um arquivo nem a recém-criada.
// - Formatação sem perda: FormatSaveData move a pasta para
//   <save>-backups/format-<AAAAMMDD-HHMMSS>/ antes de recriar.

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace Oot3dNativeGame {

inline constexpr std::string_view kCtrSaveJournalTemporarySuffix = ".triaevum-tmp";
inline constexpr std::string_view kCtrSaveJournalCommittedSuffix = ".triaevum-new";
inline constexpr std::string_view kCtrSaveJournalPreviousSuffix = ".triaevum-old";
// Ordem de criação das gerações (um nome por linha), dentro de <save>-backups.
inline constexpr std::string_view kCtrSaveBackupOrderFile = ".triaevum-geracoes";
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
// tenha a única cópia de algum arquivo nem keepGeneration (a recém-criada).
// Só toca pastas AAAAMMDD-HHMMSS[-n]. Ordem: as que não estão em
// kCtrSaveBackupOrderFile (anteriores ao arquivo de ordem) por nome, depois
// as listadas na ordem do arquivo. Sobra de cópia (*.triaevum-tmp) não conta.
void PruneCtrSaveBackups(const std::filesystem::path& backupRoot, size_t keep,
                         const std::filesystem::path& keepGeneration = {});
// Troca destination por source. Tenta rename; se o destino existe e o rename
// não sobrescreve (Horizon: EEXIST), o destino vira <destino>.triaevum-old, o
// source entra e o .old é removido; se o rename do source falha, o .old volta
// para o lugar. destination nunca fica sem nenhuma das duas versões no disco.
bool ReplaceCtrSaveFile(const std::filesystem::path& source, const std::filesystem::path& destination);
// Testes no PC: simula o rename do Horizon (destino existente -> EEXIST).
void SetCtrSaveRenameFailsOnExistingForTesting(bool enabled) noexcept;
// Testes: o rename de from para to falha (EIO) quando fault devolve true.
void SetCtrSaveRenameFaultForTesting(
    std::function<bool(const std::filesystem::path& from, const std::filesystem::path& to)> fault);
// Testes: chamado depois de gravar cada cópia (backup ou temporário), antes
// da conferência; permite estragar a cópia (SD que grava errado).
void SetCtrSaveCopyFaultForTesting(std::function<void(const std::filesystem::path& destination)> fault);

// Estado de uma execução sobre uma pasta de save (sobrevive ao restore de
// savestate: o host repassa o mesmo objeto).
class CtrSaveJournal {
  public:
    explicit CtrSaveJournal(std::filesystem::path saveDirectory);

    const std::filesystem::path& Directory() const noexcept { return mDirectory; }
    // file é arquivo do save (e não de extdata/RomFS)?
    bool Covers(const std::filesystem::path& file) const;
    // Uma vez por execução: termina trocas interrompidas e põe de lado os
    // temporários não confirmados. O host chama na criação do diário, ou
    // seja, no primeiro acesso ao save da execução (inclusive restore).
    void RecoverOnce();
    // Onde ler/escrever agora: o temporário se há escrita pendente; o
    // .triaevum-new se a troca desta execução não conseguiu instalá-lo.
    std::filesystem::path ActivePath(const std::filesystem::path& file) const;
    // Antes de escrever/redimensionar: backup (1x por execução), cópia para o
    // temporário (1x por transação) e registro do handle como escritor.
    // writer = identificador do handle (0: o diário atribui um novo).
    bool BeginWrite(const std::filesystem::path& file, uint64_t& writer);
    // Write/SetSize falhou depois do BeginWrite: a transação não é instalada.
    void MarkFailed(const std::filesystem::path& file);
    // FSFILE_Flush do handle writer: confirma se ele é o único escritor.
    bool Flush(const std::filesystem::path& file, uint64_t writer);
    // FSFILE_Close/svcCloseHandle do handle writer: confirma se era o último.
    bool Close(const std::filesystem::path& file, uint64_t writer);
    // ControlArchive(commit): confirma todas (falha se alguma falhou).
    bool CommitAll();
    // Restore de savestate: esquece escritores que não existem mais.
    void RetainWriters(const std::set<uint64_t>& writers);
    // DeleteFile: guarda a versão atual nos backups e descarta a transação.
    bool PrepareDelete(const std::filesystem::path& file);
    // FormatSaveData: confirma o pendente e move a pasta (se tem algo) para
    // <save>-backups/format-<data>. Falso = não formatar.
    bool MoveSaveAsideForFormat();

  private:
    struct Transaction {
        std::set<uint64_t> Writers;
        bool Failed = false;
    };
    std::string Key(const std::filesystem::path& file) const;
    bool Backup(const std::filesystem::path& file, bool beforeDelete);
    std::filesystem::path Generation();
    // Confirma a transação (ou, se falhou, põe o temporário de lado).
    bool CommitNow(const std::string& key);
    // Termina a instalação de um .triaevum-new que ficou para trás.
    bool InstallStranded(const std::string& key);

    std::filesystem::path mDirectory;
    std::filesystem::path mBackupRoot;
    std::filesystem::path mGeneration;
    bool mGenerationRegistered = false;
    std::map<std::string, Transaction> mPending;
    std::set<std::string> mStranded;
    std::set<std::string> mBackedUp;
    uint64_t mNextWriter = 1U;
    bool mRecovered = false;
};

} // namespace Oot3dNativeGame
