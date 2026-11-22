#include "StdInclude.hpp"
#include "Cursor.hpp"

#include "UI/UIManager.hpp"

namespace IWXMVM::T6::Cursor
{
    namespace
    {
        using ShowCursor_t = int(WINAPI*)(BOOL);
        ShowCursor_t realShowCursor = nullptr;

        // calls the game made while the editor was up, for the log
        std::atomic<std::uint32_t> heldCalls = 0;
        std::atomic<std::uint64_t> lastLog = 0;

        bool EditorShown()
        {
            auto& ui = UI::UIManager::Get();
            return ui.IsInitialized() && !ui.IsOverlayHidden();
        }

        int WINAPI ShowCursor_Hook(BOOL show)
        {
            if (!EditorShown())
                return realShowCursor(show);

            const auto calls = ++heldCalls;
            const auto now = GetTickCount64();
            if (now - lastLog.load() >= 10000)
            {
                lastLog = now;
                LOG_DEBUG("T6: {} of the game's ShowCursor calls kept from Windows while the editor is shown", calls);
            }
            return show ? 0 : -1;
        }

        // the game's import slot for user32!ShowCursor
        void** FindImport(HMODULE module, const char* dll, const char* function)
        {
            const auto base = reinterpret_cast<std::uint8_t*>(module);
            const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            if (!directory.VirtualAddress)
                return nullptr;
            for (auto import = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
                 import->Name; ++import)
            {
                if (_stricmp(reinterpret_cast<const char*>(base + import->Name), dll) != 0)
                    continue;
                auto names = reinterpret_cast<const IMAGE_THUNK_DATA*>(
                    base + (import->OriginalFirstThunk ? import->OriginalFirstThunk : import->FirstThunk));
                auto slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + import->FirstThunk);
                for (; names->u1.AddressOfData; ++names, ++slots)
                {
                    if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                        continue;
                    const auto byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                    if (std::strcmp(reinterpret_cast<const char*>(byName->Name), function) == 0)
                        return reinterpret_cast<void**>(&slots->u1.Function);
                }
            }
            return nullptr;
        }
    }  // namespace

    void Install()
    {
        const auto slot = FindImport(GetModuleHandleA(nullptr), "user32.dll", "ShowCursor");
        if (!slot)
        {
            LOG_WARN("T6: the game's import of ShowCursor wasn't found; the cursor is left to the game");
            return;
        }
        DWORD protection = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protection))
            return;
        realShowCursor = reinterpret_cast<ShowCursor_t>(*slot);
        *slot = reinterpret_cast<void*>(&ShowCursor_Hook);
        VirtualProtect(slot, sizeof(void*), protection, &protection);
        LOG_INFO("T6: the game's ShowCursor calls go through T6iris (import at {:#x})",
                 reinterpret_cast<std::uintptr_t>(slot));
    }
}  // namespace IWXMVM::T6::Cursor
