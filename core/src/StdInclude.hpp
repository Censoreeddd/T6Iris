#pragma once

#define _CRT_SECURE_NO_WARNINGS

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <variant>
#include <stack>
#include <chrono>

#include <initguid.h>
#include <d3d9.h>
#pragma comment(lib, "d3d9.lib")
#include <d3dx9.h>
#pragma comment(lib, "d3dx9.lib")
#include <d3dx9tex.h>

// T6 renders with Direct3D 11 (see D3D11.hpp). DXGI and D3D11 come from the Windows SDK, not the June 2010 SDK, whose
// include folder is searched first: its dxgi.h is older than what TexturePack's <dxgi1_4.h> needs (DXGI_RGBA,
// DXGI_COLOR_SPACE_TYPE), and a header once included isn't read again. Only the Windows SDK has these two, and they
// bring its own dxgi.h, d3d11.h and d3dcommon.h from their folders before the 2010 SDK's can be found. d3dx11 is the
// 2010 SDK's (the Windows SDK has none); its includes of those headers are then skipped.
#pragma warning(push)
#pragma warning(disable : 4005)
#include <dxgi1_4.h>
#include <d3d11_1.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#include <d3dx11.h>
#pragma comment(lib, "d3dx11.lib")
#pragma warning(pop)

#include <Windows.h>
#include <psapi.h>
#include <shobjidl.h>
#include <Propkey.h>

#include "Logger.hpp"

#include "glm/glm.hpp"
#include "glm/ext.hpp"
#include "glm/gtx/euler_angles.hpp"
#include "glm/gtx/scalar_multiplication.hpp"
#include "glm/gtx/vector_angle.hpp"
#include "glm/gtx/rotate_vector.hpp"
#include "glm/gtx/spline.hpp"
#include "glm/gtx/intersect.hpp"
#include "Utilities/GLMExtensions.hpp"

#pragma warning(push, 0)
#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#pragma warning(pop)

#include "magic_enum/magic_enum.hpp"
