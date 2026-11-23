#pragma once
// SHBIN (DVLB) -> the packet and decoded state a draw of each vertex DVLE would
// have, from the DVLE output table alone (no capture). Same rules as
// tools/renderer/tev_program/build_vertex_artifacts.cpp; geometry DVLEs are
// listed, not translated (the deko3d backend has no geometry stage).
#include "oot3d_native_pica_draw_state.h"
#include "oot3d_native_pica_frontend.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Oot3dNativeGame {

struct Oot3dShbinVertexEntry {
    size_t Dvle = 0;
    std::unique_ptr<Oot3dPicaDrawPacket> Packet;
    Oot3dPicaDecodedDrawState State{};
};

struct Oot3dShbinEntries {
    std::vector<Oot3dShbinVertexEntry> Vertex;
    std::vector<size_t> Geometry;
};

bool ReadOot3dShbinEntries(std::span<const uint8_t> bytes, Oot3dShbinEntries& entries,
                           std::string* error = nullptr);

} // namespace Oot3dNativeGame
