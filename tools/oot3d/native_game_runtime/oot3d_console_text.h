#pragma once

// The start-up screens (oot3d_switch_console_screen.h) are written in UTF-8
// PT-BR, but libnx's console prints one glyph per byte from its default font,
// code page 437. That font has the lowercase acute, circumflex and grave
// vowels, c with cedilla, C, E and u with diaeresis; it has no a/o with tilde
// nor most uppercase accents, which keep their base letter. Anything else
// outside ASCII is "?". Plain code: host-tested (oot3d_console_text_tests).

#include <string>
#include <string_view>

namespace Oot3dNativeGame {

inline std::string ToConsoleText(std::string_view utf8) {
    std::string text;
    text.reserve(utf8.size());
    for (std::size_t i = 0; i < utf8.size(); ++i) {
        const auto lead = static_cast<unsigned char>(utf8[i]);
        if (lead < 0x80) {
            text.push_back(static_cast<char>(lead));
            continue;
        }
        // Latin-1 letters are two bytes, C3 xx; skip the rest of any other sequence.
        if (lead == 0xC3 && i + 1 < utf8.size()) {
            const auto letter = static_cast<unsigned char>(utf8[++i]);
            switch (letter) {
            case 0xA1: text.push_back('\xA0'); continue; // á
            case 0xA9: text.push_back('\x82'); continue; // é
            case 0xAD: text.push_back('\xA1'); continue; // í
            case 0xB3: text.push_back('\xA2'); continue; // ó
            case 0xBA: text.push_back('\xA3'); continue; // ú
            case 0xA2: text.push_back('\x83'); continue; // â
            case 0xAA: text.push_back('\x88'); continue; // ê
            case 0xB4: text.push_back('\x93'); continue; // ô
            case 0xA0: text.push_back('\x85'); continue; // à
            case 0xA7: text.push_back('\x87'); continue; // ç
            case 0x87: text.push_back('\x80'); continue; // Ç
            case 0x89: text.push_back('\x90'); continue; // É
            case 0xBC: text.push_back('\x81'); continue; // ü
            case 0xA3: text.push_back('a'); continue;    // ã
            case 0xB5: text.push_back('o'); continue;    // õ
            case 0x80: case 0x81: case 0x82: case 0x83: text.push_back('A'); continue;
            case 0x8A: text.push_back('E'); continue;
            case 0x8D: text.push_back('I'); continue;
            case 0x93: case 0x94: case 0x95: text.push_back('O'); continue;
            case 0x9A: text.push_back('U'); continue;
            default: break;
            }
            text.push_back('?');
            continue;
        }
        text.push_back('?');
        while (i + 1 < utf8.size() && (static_cast<unsigned char>(utf8[i + 1]) & 0xC0) == 0x80)
            ++i;
    }
    return text;
}

} // namespace Oot3dNativeGame
