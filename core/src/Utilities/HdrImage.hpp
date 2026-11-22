#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// High dynamic range images, for the Sky: Radiance .hdr (RGBE) and OpenEXR .exr (tinyexr), as linear light RGBA
// floats, top row first
namespace IWXMVM::HdrImage
{
    struct Image
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::vector<float> rgba;  // width * height * 4
    };

    // whether the file's one (by its extension)
    bool IsHdr(const std::filesystem::path& path);
    // the image, or why it couldn't be read
    std::optional<Image> Load(const std::filesystem::path& path, std::string& error);
    // halved (2x2 averaged) until it's at most so wide
    void ShrinkTo(Image& image, std::uint32_t maxWidth);
    // a float as a half float's bits (for a 16 bit float texture)
    std::uint16_t ToHalf(float value);
}  // namespace IWXMVM::HdrImage
