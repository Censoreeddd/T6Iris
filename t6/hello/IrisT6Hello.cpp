// IrisT6Hello: the smallest Redacted T6 plugin. Run it once before T6iris: it shows that Redacted loads the plugin, in
// what order it calls the lifecycle functions, and when the game's window and d3d9.dll appear relative to them. It
// hooks and patches nothing.
//
// Output: IrisT6Hello.log beside the game's executable, and the debugger's output (OutputDebugString).
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <intrin.h>
#include <psapi.h>

#ifdef _MSC_VER
#define IRIS_RETURN_ADDRESS() _ReturnAddress()
#else
#define IRIS_RETURN_ADDRESS() __builtin_return_address(0)
#endif

namespace
{
    FILE* logFile = nullptr;
    DWORD startTick = 0;
    CRITICAL_SECTION logLock;

    void Log(const char* format, ...)
    {
        char line[512];
        va_list args;
        va_start(args, format);
        vsnprintf(line, sizeof(line), format, args);
        va_end(args);

        char stamped[640];
        snprintf(stamped, sizeof(stamped), "[+%7lu ms][thread %5lu] %s\n", GetTickCount() - startTick,
                 GetCurrentThreadId(), line);
        OutputDebugStringA(stamped);
        EnterCriticalSection(&logLock);
        if (logFile)
        {
            fputs(stamped, logFile);
            fflush(logFile);
        }
        LeaveCriticalSection(&logLock);
    }

    // ---- Which exports does the loader ask us for? ----------------------------------------------------------
    // Every module loaded so far gets its import of GetProcAddress pointed here. Lookups of anything in this plugin
    // are logged with the module that made them; the rest pass straight through.
    HMODULE thisModule = nullptr;
    typedef FARPROC(WINAPI* GetProcAddress_t)(HMODULE, LPCSTR);
    GetProcAddress_t realGetProcAddress = nullptr;

    void ModuleNameOf(const void* address, char* name, DWORD size)
    {
        HMODULE module = nullptr;
        strcpy_s(name, size, "?");
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(address), &module))
        {
            char path[MAX_PATH]{};
            GetModuleFileNameA(module, path, MAX_PATH);
            const char* slash = strrchr(path, '\\');
            strcpy_s(name, size, slash ? slash + 1 : path);
        }
    }

    FARPROC WINAPI LoggingGetProcAddress(HMODULE module, LPCSTR name)
    {
        const FARPROC result = realGetProcAddress(module, name);
        if (module == thisModule)
        {
            char caller[MAX_PATH];
            ModuleNameOf(IRIS_RETURN_ADDRESS(), caller, MAX_PATH);
            if (reinterpret_cast<uintptr_t>(name) >> 16)
                Log("loader (%s) asked for export '%s': %s", caller, name, result ? "found" : "MISSING");
            else
                Log("loader (%s) asked for export ordinal %u: %s", caller,
                    static_cast<unsigned>(reinterpret_cast<uintptr_t>(name)), result ? "found" : "MISSING");
        }
        return result;
    }

    int PatchImports(HMODULE module, const void* target)
    {
        auto base = reinterpret_cast<BYTE*>(module);
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return 0;
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!directory.VirtualAddress)
            return 0;

        int patched = 0;
        for (auto import = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress); import->Name;
             ++import)
        {
            for (auto thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + import->FirstThunk); thunk->u1.Function;
                 ++thunk)
            {
                auto slot = reinterpret_cast<void**>(&thunk->u1.Function);
                if (*slot != target)
                    continue;
                DWORD protect = 0;
                if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protect))
                {
                    *slot = reinterpret_cast<void*>(&LoggingGetProcAddress);
                    VirtualProtect(slot, sizeof(void*), protect, &protect);
                    ++patched;
                }
            }
        }
        return patched;
    }

    void InstallExportLogger()
    {
        realGetProcAddress = reinterpret_cast<GetProcAddress_t>(
            GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetProcAddress"));
        const void* targets[] = {reinterpret_cast<void*>(realGetProcAddress),
                                 GetModuleHandleA("kernelbase.dll")
                                     ? reinterpret_cast<void*>(
                                           GetProcAddress(GetModuleHandleA("kernelbase.dll"), "GetProcAddress"))
                                     : nullptr};

        HMODULE modules[1024];
        DWORD needed = 0;
        if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
            return;
        const DWORD count = needed / sizeof(HMODULE) < 1024 ? needed / sizeof(HMODULE) : 1024;
        for (DWORD i = 0; i < count; ++i)
        {
            if (modules[i] == thisModule)
                continue;
            int patched = 0;
            for (const void* target : targets)
                if (target)
                    patched += PatchImports(modules[i], target);
            if (patched)
            {
                char name[MAX_PATH]{};
                GetModuleFileNameA(modules[i], name, MAX_PATH);
                Log("watching GetProcAddress in %s", name);
            }
        }
    }

    struct WindowSearch
    {
        DWORD processId;
        HWND found;
    };

    BOOL CALLBACK FindWindowOfProcess(HWND window, LPARAM parameter)
    {
        auto& search = *reinterpret_cast<WindowSearch*>(parameter);
        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        if (processId != search.processId || !IsWindowVisible(window) || GetWindow(window, GW_OWNER))
            return TRUE;
        char className[256]{};
        GetClassNameA(window, className, sizeof(className));
        if (_stricmp(className, "ConsoleWindowClass") == 0)
            return TRUE;
        search.found = window;
        return FALSE;
    }

    HWND FindGameWindow()
    {
        WindowSearch search{GetCurrentProcessId(), nullptr};
        EnumWindows(FindWindowOfProcess, reinterpret_cast<LPARAM>(&search));
        return search.found;
    }

    void LogState(const char* where)
    {
        const HWND window = FindGameWindow();
        char className[256] = "-";
        char title[256] = "-";
        if (window)
        {
            GetClassNameA(window, className, sizeof(className));
            GetWindowTextA(window, title, sizeof(title));
        }
        Log("%s: d3d9.dll %s; game window %s (class '%s', title '%s', %s)", where,
            GetModuleHandleA("d3d9.dll") ? "loaded" : "NOT loaded", window ? "found" : "NOT found", className, title,
            window ? (IsWindowUnicode(window) ? "unicode" : "ansi") : "-");
    }

    // Reports when d3d9.dll and the game's window first appear, for up to two minutes
    DWORD WINAPI Watcher(LPVOID)
    {
        bool d3d9 = false, window = false;
        for (int i = 0; i < 1200 && !(d3d9 && window); ++i)
        {
            if (!d3d9 && GetModuleHandleA("d3d9.dll"))
            {
                d3d9 = true;
                LogState("watcher: d3d9.dll appeared");
            }
            if (!window && FindGameWindow())
            {
                window = true;
                LogState("watcher: game window appeared");
            }
            Sleep(100);
        }
        if (!(d3d9 && window))
            LogState("watcher: gave up after 120 s");
        return 0;
    }
}  // namespace

BOOL APIENTRY DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        InitializeCriticalSection(&logLock);
        startTick = GetTickCount();
        char path[MAX_PATH]{};
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        if (char* slash = strrchr(path, '\\'))
            *(slash + 1) = '\0';
        strcat_s(path, "IrisT6Hello.log");
        logFile = fopen(path, "w");
        thisModule = instance;
        char exe[MAX_PATH]{};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        Log("DllMain(PROCESS_ATTACH) in %s", exe);
        InstallExportLogger();
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        // reserved is null when the plugin is unloaded with FreeLibrary, and set when the process is ending
        Log("DllMain(PROCESS_DETACH): %s", reserved ? "the process is exiting" : "FreeLibrary unloaded the plugin");
        if (logFile)
            fclose(logFile);
        logFile = nullptr;
    }
    return TRUE;
}

// The functions Redacted calls, as its own ExtendedConsole.Red32n exports them (cdecl, plain names via the .def)
extern "C"
{
    int __cdecl PreInit()
    {
        char exe[MAX_PATH]{};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        Log("PreInit: executable %s", exe);
        LogState("PreInit");
        CreateThread(nullptr, 0, Watcher, nullptr, 0, nullptr);
        return 1;
    }

    int __cdecl PostInit()
    {
        LogState("PostInit");
        return 1;
    }

    int __cdecl Uninit()
    {
        Log("Uninit");
        return 1;
    }

    // ExtendedConsole.Red32n exports this with the same body as PostInit (return 1), so it may be part of the
    // plugin interface. Its arguments are unknown; cdecl leaves the stack to the caller, so taking none is safe.
    int __cdecl SendMessageB()
    {
        Log("SendMessageB called");
        return 1;
    }

    int __cdecl OfficialMod()
    {
        Log("OfficialMod asked");
        return 0;
    }

    const char* __cdecl AuthorInfo()
    {
        Log("AuthorInfo asked");
        return "Iris Studio contributors";
    }

    const char* __cdecl ExtendedInfo()
    {
        Log("ExtendedInfo asked");
        return "IrisT6Hello: logs the plugin lifecycle only";
    }

    int __cdecl DependencyCount()
    {
        Log("DependencyCount asked");
        return 0;
    }

    const char* __cdecl GetDependency(int index)
    {
        Log("GetDependency(%d) asked", index);
        return "";
    }
}
