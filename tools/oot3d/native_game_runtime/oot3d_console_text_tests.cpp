// Host tests for ToConsoleText (oot3d_console_text.h): the start-up screens
// are written in UTF-8 PT-BR and libnx's console font is code page 437.
#include "oot3d_console_text.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
int gFailures = 0;

void Require(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++gFailures;
    }
}
} // namespace

int main() {
    using Oot3dNativeGame::ToConsoleText;
    Require(ToConsoleText("Ocarina of Time 3D\n[####....] 50%") == "Ocarina of Time 3D\n[####....] 50%",
            "ASCII passes through");
    // CP437: á=A0 é=82 í=A1 ó=A2 ú=A3 â=83 ê=88 ô=93 à=85 ç=87 Ç=80 É=90 ü=81.
    Require(ToConsoleText("\xC3\xA1\xC3\xA9\xC3\xAD\xC3\xB3\xC3\xBA") == "\xA0\x82\xA1\xA2\xA3",
            "acute vowels map to code page 437");
    Require(ToConsoleText("\xC3\xA2\xC3\xAA\xC3\xB4\xC3\xA0\xC3\xA7\xC3\x87\xC3\x89\xC3\xBC") ==
                "\x83\x88\x93\x85\x87\x80\x90\x81",
            "circumflex, grave, cedilla and uppercase letters the font has");
    // The font has no a/o with tilde nor most uppercase accents: the base letter.
    Require(ToConsoleText("importa\xC3\xA7\xC3\xA3o n\xC3\xA3o p\xC3\xB5" "e") == "importa\x87" "ao nao poe",
            "a and o with tilde lose the tilde");
    Require(ToConsoleText("\xC3\x83\xC3\x95\xC3\x81\xC3\x8D\xC3\x93\xC3\x9A\xC3\x82\xC3\x8A\xC3\x94\xC3\x80") ==
                "AOAIOUAEOA",
            "uppercase accents the font lacks keep the base letter");
    Require(ToConsoleText("\xE2\x80\x94 \xE2\x80\x9Cx\xE2\x80\x9D \xC3") == "? ?x? ?",
            "anything else, and a cut sequence, is one ?");
    if (gFailures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", gFailures);
        return EXIT_FAILURE;
    }
    std::puts("oot3d_console_text_tests: ok");
    return EXIT_SUCCESS;
}
