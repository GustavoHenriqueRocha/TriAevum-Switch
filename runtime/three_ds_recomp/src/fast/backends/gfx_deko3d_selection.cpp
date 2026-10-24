// config/renderer: which rendering backend the Switch build uses
// (gfx_deko3d.h). Built on every platform; only the Switch reads the file.
#include "fast/backends/gfx_deko3d.h"

#include <fstream>
#include <iterator>
#include <string>

namespace Fast {

bool Deko3dRendererSelected() {
#if defined(__SWITCH__)
    static const bool selected = [] {
        std::ifstream file("sdmc:/switch/oot3dre/config/renderer");
        if (!file) {
            return false;
        }
        const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        return Deko3dRendererConfigSelects(content);
    }();
    return selected;
#else
    return false;
#endif
}

} // namespace Fast
