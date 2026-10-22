#pragma once

// UI text that names PC keys: the console build (controller only, no
// keyboard shortcuts) shows its own wording instead.
#if defined(__SWITCH__)
#define TRIAEVUM_KEY_HINT(console, pc) console
#else
#define TRIAEVUM_KEY_HINT(console, pc) pc
#endif
