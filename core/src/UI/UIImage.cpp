#include "StdInclude.hpp"
#include "UIImage.hpp"

#include "Mod.hpp"
#include "Renderer.hpp"

namespace IWXMVM::UI
{
    ImVec2 UIImage::GetDimensions() const
    {
        return dimensions;
    }

    ImTextureID UIImage::GetTextureID() const
    {
        return texture;
    }

    std::unordered_map<uint8_t*, UIImage> resourceImageCache;

    UIImage UIImage::FromResource(const uint8_t data[], uint32_t size)
    {
        if (resourceImageCache.contains((uint8_t*)data))
            return resourceImageCache[(uint8_t*)data];

        UIImage image;

        image.texture = Renderer::CreateTextureFromMemory(data, size, image.dimensions);

        resourceImageCache.insert({(uint8_t*)data, image});

        return image;
    }

    void UIImage::ReleaseResource(const uint8_t data[])
    {
        try
        {
            auto search = resourceImageCache.find((uint8_t*)data);

            if (search != resourceImageCache.end())
            {
                Renderer::ReleaseTexture(search->second.texture);

                resourceImageCache.erase(search);
            }
        }
        catch (...)
        {
            // TODO: searching the resource container fails when closing the game
            // potential memory leak because the textures are not released

            // possible solution: move ownership away from global container to class instance of ControlBar
        }
    }
}  // namespace IWXMVM::UI