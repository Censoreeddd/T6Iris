#pragma once

namespace IWXMVM::Types
{
    // The graphics API a game renders with, which decides how the editor draws over it (see Renderer)
    enum class GraphicsApi
    {
        D3D9,   // IW3, IW5, T5
        D3D11,  // T6
    };
}  // namespace IWXMVM::Types
