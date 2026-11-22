#include "HdrImage.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

#include "tinyexr.h"

namespace IWXMVM::HdrImage
{
    namespace
    {
        std::string Extension(const std::filesystem::path& path)
        {
            auto extension = path.extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return extension;
        }

        // A Radiance file: a header to an empty line, the size ("-Y height +X width"), then its scanlines of RGBE
        // pixels, each run-length encoded (by channel) or flat
        std::optional<Image> LoadRadiance(const std::filesystem::path& path, std::string& error)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
            {
                error = "couldn't open it";
                return std::nullopt;
            }
            std::string line;
            if (!std::getline(file, line) || line.rfind("#?", 0) != 0)
            {
                error = "not a Radiance file";
                return std::nullopt;
            }
            while (std::getline(file, line) && !line.empty() && line != "\r")
            {
                if (line.rfind("FORMAT=", 0) == 0 && line.find("32-bit_rle_rgbe") == std::string::npos)
                {
                    error = "only RGBE Radiance files are read";
                    return std::nullopt;
                }
            }
            if (!std::getline(file, line))
            {
                error = "no size";
                return std::nullopt;
            }
            std::istringstream size(line);
            std::string yAxis, xAxis;
            int height = 0, width = 0;
            size >> yAxis >> height >> xAxis >> width;
            if (yAxis != "-Y" || xAxis != "+X" || width <= 0 || height <= 0 || width > 32768 || height > 32768)
            {
                error = "an unusual size or orientation (" + line + ")";
                return std::nullopt;
            }

            Image image;
            image.width = static_cast<std::uint32_t>(width);
            image.height = static_cast<std::uint32_t>(height);
            image.rgba.resize(static_cast<std::size_t>(width) * height * 4);
            std::vector<std::uint8_t> scanline(static_cast<std::size_t>(width) * 4);
            for (int y = 0; y < height; ++y)
            {
                std::uint8_t start[4];
                if (!file.read(reinterpret_cast<char*>(start), 4))
                {
                    error = "cut short";
                    return std::nullopt;
                }
                if (width >= 8 && width < 32768 && start[0] == 2 && start[1] == 2 &&
                    ((start[2] << 8) | start[3]) == width)
                {
                    // run-length encoded: each channel in turn
                    for (int channel = 0; channel < 4; ++channel)
                    {
                        int x = 0;
                        while (x < width)
                        {
                            const int count = file.get();
                            if (count == EOF)
                            {
                                error = "cut short";
                                return std::nullopt;
                            }
                            if (count > 128)
                            {
                                const int value = file.get();
                                const int run = count - 128;
                                if (value == EOF || x + run > width)
                                {
                                    error = "a bad run";
                                    return std::nullopt;
                                }
                                for (int i = 0; i < run; ++i)
                                    scanline[static_cast<std::size_t>(x++) * 4 + channel] = static_cast<std::uint8_t>(value);
                            }
                            else
                            {
                                if (count == 0 || x + count > width)
                                {
                                    error = "a bad run";
                                    return std::nullopt;
                                }
                                for (int i = 0; i < count; ++i)
                                {
                                    const int value = file.get();
                                    if (value == EOF)
                                    {
                                        error = "cut short";
                                        return std::nullopt;
                                    }
                                    scanline[static_cast<std::size_t>(x++) * 4 + channel] = static_cast<std::uint8_t>(value);
                                }
                            }
                        }
                    }
                }
                else
                {
                    // flat: the four bytes read, then the rest
                    std::memcpy(scanline.data(), start, 4);
                    if (!file.read(reinterpret_cast<char*>(scanline.data()) + 4, static_cast<std::streamsize>(width) * 4 - 4))
                    {
                        error = "cut short";
                        return std::nullopt;
                    }
                }
                for (int x = 0; x < width; ++x)
                {
                    const auto* rgbe = &scanline[static_cast<std::size_t>(x) * 4];
                    const float scale = rgbe[3] ? std::ldexp(1.0f, static_cast<int>(rgbe[3]) - (128 + 8)) : 0.0f;
                    auto* out = &image.rgba[(static_cast<std::size_t>(y) * width + x) * 4];
                    out[0] = (rgbe[0] + 0.5f) * scale;
                    out[1] = (rgbe[1] + 0.5f) * scale;
                    out[2] = (rgbe[2] + 0.5f) * scale;
                    out[3] = 1.0f;
                    if (!rgbe[3])
                        out[0] = out[1] = out[2] = 0.0f;
                }
            }
            return image;
        }

        std::optional<Image> LoadOpenExr(const std::filesystem::path& path, std::string& error)
        {
            float* rgba = nullptr;
            int width = 0, height = 0;
            const char* message = nullptr;
            const auto name = path.string();
            if (LoadEXR(&rgba, &width, &height, name.c_str(), &message) != TINYEXR_SUCCESS || !rgba)
            {
                error = message ? message : "tinyexr couldn't read it";
                if (message)
                    FreeEXRErrorMessage(message);
                std::free(rgba);
                return std::nullopt;
            }
            Image image;
            image.width = static_cast<std::uint32_t>(width);
            image.height = static_cast<std::uint32_t>(height);
            image.rgba.assign(rgba, rgba + static_cast<std::size_t>(width) * height * 4);
            std::free(rgba);
            // (a sky is opaque, whatever its alpha says)
            for (std::size_t i = 3; i < image.rgba.size(); i += 4)
                image.rgba[i] = 1.0f;
            return image;
        }
    }  // namespace

    bool IsHdr(const std::filesystem::path& path)
    {
        const auto extension = Extension(path);
        return extension == ".hdr" || extension == ".exr";
    }

    std::optional<Image> Load(const std::filesystem::path& path, std::string& error)
    {
        return Extension(path) == ".exr" ? LoadOpenExr(path, error) : LoadRadiance(path, error);
    }

    void ShrinkTo(Image& image, std::uint32_t maxWidth)
    {
        while (image.width > maxWidth && image.width >= 2 && image.height >= 2)
        {
            const auto width = image.width / 2, height = image.height / 2;
            std::vector<float> smaller(static_cast<std::size_t>(width) * height * 4);
            for (std::uint32_t y = 0; y < height; ++y)
                for (std::uint32_t x = 0; x < width; ++x)
                    for (int c = 0; c < 4; ++c)
                    {
                        const auto at = [&](std::uint32_t sx, std::uint32_t sy) {
                            return image.rgba[(static_cast<std::size_t>(sy) * image.width + sx) * 4 + c];
                        };
                        smaller[(static_cast<std::size_t>(y) * width + x) * 4 + c] =
                            0.25f * (at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) +
                                     at(2 * x + 1, 2 * y + 1));
                    }
            image.width = width;
            image.height = height;
            image.rgba = std::move(smaller);
        }
    }

    std::uint16_t ToHalf(float value)
    {
        std::uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        const std::uint32_t sign = (bits >> 16) & 0x8000u;
        const std::int32_t exponent = static_cast<std::int32_t>((bits >> 23) & 0xFF) - 127 + 15;
        const std::uint32_t mantissa = bits & 0x7FFFFFu;
        if (((bits >> 23) & 0xFF) == 0xFF)  // infinity or NaN
            return static_cast<std::uint16_t>(sign | 0x7C00u | (mantissa ? 0x200u : 0u));
        if (exponent >= 31)  // too big: the largest half
            return static_cast<std::uint16_t>(sign | 0x7BFFu);
        if (exponent <= 0)
        {
            if (exponent < -10)
                return static_cast<std::uint16_t>(sign);
            // subnormal
            const std::uint32_t m = (mantissa | 0x800000u) >> (1 - exponent);
            return static_cast<std::uint16_t>(sign | ((m + 0x1000u) >> 13));
        }
        std::uint32_t half = sign | (static_cast<std::uint32_t>(exponent) << 10) | (mantissa >> 13);
        // (rounded to nearest)
        if (mantissa & 0x1000u)
            ++half;
        return static_cast<std::uint16_t>(half);
    }
}  // namespace IWXMVM::HdrImage
