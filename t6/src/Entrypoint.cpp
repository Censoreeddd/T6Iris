#include "StdInclude.hpp"
#include "Mod.hpp"

#include "T6Interface.hpp"

// T6iris is a Redacted plugin: the Redacted client loads every *.Red32n in the game's Plugins folder (a plain DLL with
// another extension) and calls the functions exported below, which T6iris.def exports by these plain names. The set
// and their behaviour mirror Redacted's own ExtendedConsole.Red32n: cdecl, PreInit does the work and returns 1 (0
// refuses the plugin), the rest return 1. Redacted.ini's [Plugins] AllowUnofficial = 1 lets an unofficial plugin load.
//
// Unverified until the hello plugin (t6/hello) has been run: when PreInit and PostInit are called relative to the
// game's window and device. Nothing here depends on it, as the mod's thread waits for both.

using namespace IWXMVM;
using namespace IWXMVM::T6;

T6Interface gameInterface = T6Interface();

namespace
{
    std::atomic<bool> started = false;

    // Only t6mp: the Zombies and campaign executables (t6zm, t6sp) load the same plugins, and the binding knows
    // nothing of them. Redacted may run a renamed copy (t6mpv43.exe), so the name is matched by its start.
    bool IsMultiplayerExecutable()
    {
        char exePath[MAX_PATH]{};
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        const auto name = std::filesystem::path(exePath).filename().string();
        return _strnicmp(name.c_str(), "t6mp", 4) == 0;
    }

    DWORD WINAPI Start(LPVOID)
    {
        // Mod::Initialize runs for as long as the mod is loaded
        Mod::Initialize(&gameInterface);
        return 0;
    }
}  // namespace

extern "C"
{
    int __cdecl PreInit()
    {
        if (!IsMultiplayerExecutable())
            return 1;  // load quietly and do nothing outside multiplayer

        bool expected = false;
        if (started.compare_exchange_strong(expected, true))
        {
            if (const auto thread = CreateThread(nullptr, 0, Start, nullptr, 0, nullptr))
                CloseHandle(thread);
        }
        return 1;
    }

    int __cdecl PostInit()
    {
        return 1;
    }

    int __cdecl Uninit()
    {
        // The game is closing. Ejecting now would race its shutdown, so the process takes the mod down with it.
        return 1;
    }

    // Exported by ExtendedConsole.Red32n with the same body as PostInit, so possibly required by the loader (see
    // t6/hello). Its arguments are unknown; cdecl leaves the stack to the caller.
    int __cdecl SendMessageB()
    {
        return 1;
    }

    int __cdecl OfficialMod()
    {
        return 0;
    }

    const char* __cdecl AuthorInfo()
    {
        return "Iris Studio contributors";
    }

    const char* __cdecl ExtendedInfo()
    {
        return "T6iris: the Iris Studio editor for Black Ops II multiplayer demos";
    }

    int __cdecl DependencyCount()
    {
        return 0;
    }

    const char* __cdecl GetDependency(int)
    {
        return "";
    }
}
