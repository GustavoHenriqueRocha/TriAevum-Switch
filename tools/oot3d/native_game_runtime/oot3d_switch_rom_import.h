#pragma once

// Importação, no primeiro início, do dump decriptado do jogador para
// sdmc:/switch/oot3dre/game/ (oot3d_rom_import.h), com uma tela de progresso
// no console do libnx. Chamada no começo do main(), no lançamento padrão do
// NRO (o do OoT3D), antes de os argumentos serem montados e de qualquer coisa
// ler o game/. config/no_rom_import desliga.

namespace Oot3dNativeGame {

// Verdadeiro para seguir o boot; falso quando o app deve sair (sem dados do
// jogo, importação que falhou sem dados anteriores, ou o jogador fechou o app).
[[nodiscard]] bool RunSwitchRomImport();

} // namespace Oot3dNativeGame
