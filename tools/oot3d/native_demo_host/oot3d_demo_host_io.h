#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include <nlohmann/json_fwd.hpp>

void WriteJsonFile(const std::filesystem::path& path, const nlohmann::json& data);

// Rows of RGB bytes, top first.
void WriteBmpFromRgb8Framebuffer(
    const std::filesystem::path& path,
    uint32_t width,
    uint32_t height,
    const std::vector<uint8_t>& rgb);

// PNG RGB de 8 bits (linhas de cima para baixo), sem dependência externa:
// deflate em blocos "stored" (sem compressão). Para capturas de diagnóstico.
void WritePngFromRgb8Framebuffer(
    const std::filesystem::path& path,
    uint32_t width,
    uint32_t height,
    const std::vector<uint8_t>& rgb);

// Mesmos bytes, em memória (testes).
std::vector<uint8_t> EncodePngFromRgb8(uint32_t width, uint32_t height,
                                       const std::vector<uint8_t>& rgb);

// O caminho pede PNG (extensão .png, sem diferenciar maiúsculas).
bool ScreenshotPathIsPng(const std::filesystem::path& path);

void WriteBmpFromRgba5551Framebuffer(
    const std::filesystem::path& path,
    uint32_t width,
    uint32_t height,
    const std::vector<uint16_t>& rgba16);

std::filesystem::path ScreenshotSequenceFramePath(const std::filesystem::path& path, uint32_t frameIndex);
