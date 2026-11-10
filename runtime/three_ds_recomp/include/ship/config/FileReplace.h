#pragma once

// Põe um arquivo temporário já gravado no lugar de uma config (ou cache).
//
// rename() do POSIX e MoveFileEx do Windows substituem um destino existente
// num passo só. O FS do Horizon (cartão SD do Switch) recusa com EEXIST. Lá o
// arquivo antigo vai antes para "<destino>.bak", o temporário assume o nome e
// o backup é apagado. Se o console desligar ou o jogo for fechado entre as duas
// trocas, o destino some e o backup guarda o último arquivo completo:
// RestoreInterruptedReplace() o põe de volta, e todo leitor de um arquivo
// gravado assim a chama antes de procurar o arquivo.
//
// Só cabeçalho, para que as bibliotecas pequenas de config (controle, UI do
// TopScreen) usem o mesmo código do Ship::WriteConfigFileAtomically sem ligar
// o runtime do ship.

#include <filesystem>
#include <system_error>

namespace Ship {

inline std::filesystem::path ReplaceBackupPath(const std::filesystem::path& target) {
    std::filesystem::path backup = target;
    backup += ".bak";
    return backup;
}

// Move `temporary` para `target`, substituindo um arquivo existente. Em caso
// de falha o destino anterior continua (ou volta) no lugar e `temporary` fica
// para quem chamou apagar.
inline std::error_code ReplaceFileWithTemporary(const std::filesystem::path& temporary,
                                                const std::filesystem::path& target) {
    std::error_code error;
    std::filesystem::rename(temporary, target, error);
    if (error != std::errc::file_exists) {
        return error;
    }
    std::error_code probe;
    if (!std::filesystem::is_regular_file(target, probe)) {
        return error;
    }
    // Horizon: sem rename por cima de arquivo existente. Um backup deixado por
    // uma troca anterior concluída está velho (o destino existe) e sai antes.
    const auto backup = ReplaceBackupPath(target);
    std::filesystem::remove(backup, probe);
    error.clear();
    std::filesystem::rename(target, backup, error);
    if (error) {
        return error;
    }
    std::filesystem::rename(temporary, target, error);
    if (error) {
        std::error_code restore;
        std::filesystem::rename(backup, target, restore);
        return error;
    }
    std::filesystem::remove(backup, probe);
    return {};
}

// Termina a recuperação de uma troca interrompida entre as duas etapas: se o
// destino não existe e o backup existe, o backup volta a ser o destino.
// Retorna false só quando uma restauração necessária falhou.
inline bool RestoreInterruptedReplace(const std::filesystem::path& target) {
    if (target.empty()) {
        return true;
    }
    const auto backup = ReplaceBackupPath(target);
    std::error_code error;
    if (!std::filesystem::is_regular_file(backup, error)) {
        return true;
    }
    if (std::filesystem::exists(target, error) || error) {
        return true;
    }
    std::filesystem::rename(backup, target, error);
    return !error;
}

} // namespace Ship
