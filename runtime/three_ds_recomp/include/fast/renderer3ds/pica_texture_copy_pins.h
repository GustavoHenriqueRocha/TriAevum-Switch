#pragma once

#include "fast/renderer3ds/pica_native_state.h"

#include <cstdint>
#include <set>
#include <span>

namespace Fast::Renderer3ds {

// Copies the presentation still references, declared by the frontend each time
// it executes a sample. Explicit pins supersede conservative latest-destination
// retention once declared. A pin names the copy by its completion id, which
// identifies every version of it across namespaces, also a version captured
// after the declaration: the presentation replay captures its own version after
// the dependency replay, and resolving pins to the versions registered at
// declaration let the next frame's sweep retire it (SM3DL, parallel
// presentation, 2026-11-06).
class PicaTextureCopyPins {
public:
    void Declare(std::span<const PicaTextureCopyReference> retained) {
        mCompletions.clear();
        for (const auto& reference : retained) mCompletions.insert(reference.CompletionId);
        mDeclared = true;
    }
    void Clear() {
        mCompletions.clear();
        mDeclared = false;
    }
    [[nodiscard]] bool Declared() const { return mDeclared; }
    [[nodiscard]] bool Retains(uint64_t completionId) const {
        return mCompletions.contains(completionId);
    }

private:
    std::set<uint64_t> mCompletions;
    bool mDeclared = false;
};

}  // namespace Fast::Renderer3ds
