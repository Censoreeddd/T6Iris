#include "StdInclude.hpp"
#include "Theater.hpp"
#include "Lights.hpp"
#include "Props.hpp"
#include "Components/CaptureManager.hpp"
#include "UI/UIManager.hpp"

#include "Addresses.hpp"
#include "Events.hpp"
#include "Functions.hpp"
#include "Structures.hpp"
#include "Components/Playback.hpp"
#include "Utilities/Inflate.hpp"
#include "Utilities/HookManager.hpp"

// The party's auto start (0x7F8EE0; the session in eax, one argument on the stack, the caller pops it): a lobby with
// party_minplayers counts down (AutoStart_LOBBY_FLAG_WAITING, session + 0x9620 4, then 0x10) and then loads the
// party's map (0x66AEE0), which in the lobby the Demo tab opened began a match under the film and crashed the game
// (0x626C88, 9 s into it). The hook returns at once while t6irisHoldAutoStart is set (Theater::OnFrame), and is
// written in assembly for the register argument.
extern "C"
{
    volatile unsigned char t6irisHoldAutoStart = 0;
    std::uintptr_t t6irisAutoStartTrampoline = 0;
}
#if defined(_MSC_VER)
static __declspec(naked) void PartyAutoStart_Hook()
{
    __asm {
        cmp byte ptr [t6irisHoldAutoStart], 0
        jne hold
        jmp dword ptr [t6irisAutoStartTrampoline]
    hold:
        ret
    }
}
#else
// (only for syntax checks with other compilers; the plugin is built with MSVC)
static void PartyAutoStart_Hook()
{
}
#endif

namespace IWXMVM::T6::Theater
{
    namespace
    {
        constexpr std::int32_t DEMO_STATE_PLAYING = 2;
        // the snapshot's time, in the structure at 0x11C7D74: it moves in steps of 100 ms (10 snapshots a second)
        constexpr std::uintptr_t CG_TIME = 0x58;
        // cg->time in the client game's cg ([0x113F18C]): the time each frame is drawn at, between snapshots, which
        // CG_CalcViewValues stamps on the refdef (cg+0x4D8FC)
        constexpr std::uintptr_t CG_VIEW_TIME = 0x4808C;
        constexpr std::size_t FILM_NAME_SIZE = 256;
        // the theater's snapshot table (RE-NOTES.md): entries of 0x30 bytes, +0x04 the offset in the .demo file
        constexpr std::uintptr_t SNAPSHOTS = 0x60580;
        constexpr std::uintptr_t SNAPSHOT_SIZE = 0x30;
        constexpr std::uintptr_t SNAPSHOT_FILE_OFFSET = 0x04;
        constexpr std::uintptr_t SNAPSHOT_COUNT = 0x63490;

        template <typename T>
        T ReadGlobal(std::uintptr_t address)
        {
            return *reinterpret_cast<const volatile T*>(address);
        }

        std::uintptr_t GetCg()
        {
            return ReadGlobal<std::uintptr_t>(GetGameAddresses().cgPointer());
        }

        std::filesystem::path GetFilmFolder()
        {
            char exePath[MAX_PATH]{};
            GetModuleFileNameA(nullptr, exePath, MAX_PATH);
            return std::filesystem::path(exePath).parent_path() / "Plugins" / "OpenNetStorage" / "Cloud" / "FILM_MP";
        }

        // the film's times, read once per file
        std::mutex timesMutex;
        std::filesystem::path timesPath;
        std::optional<FilmTimes> times;

        // the film's file, found once per film (see FindFilmFile)
        std::mutex fileMutex;
        std::filesystem::path filmFile;
        bool filmFileSearched = false;
        // when the film's file was last looked for without being found, and whether that was said
        std::uint64_t filmFileSearchedAt = 0;
        bool filmFileMissLogged = false;

        constexpr std::uintptr_t SNAPSHOT_TIME = 0x08;

        // Seeking (SeekTo): demo_back restores the last snapshot at or before a time, but not the first snapshot
        // (demo_back 660 and 400 did nothing, while 15300 went to the second's 10601), for which demo_jumptostart is
        // there; demo_forward with a time does nothing (all seen in the game). So a seek goes back with
        // demo_back when it has to, then plays forward at a raised timescale, slowing as it nears the time, and puts
        // the speed and the pause back once there.
        enum class SeekState
        {
            Idle,
            Jumping,     // demo_back sent, waiting for the film to go back
            Forwarding,  // playing on to the time
        };
        constexpr std::uint64_t SEEK_INTERVAL_MS = 80;    // how often a dragged timeline sends demo_back
        constexpr std::uint64_t JUMP_TIMEOUT_MS = 1500;   // a demo_back that didn't go back
        bool retriedJump = false;                         // the jump back was tried with the other command too
        constexpr std::uint64_t STALL_TIMEOUT_MS = 2000;  // forwarding that doesn't move (the film's end)
        // (the editor's game view keeps its frame while it plays on fast, Renderer.cpp)
        constexpr float MAX_SEEK_SPEED = 64.0f;
        // the film's milliseconds left at which the speed is down to the normal one: speed = left / SLOWDOWN_MS
        constexpr float SLOWDOWN_MS = 60.0f;
        // A seek forward jumps to a snapshot already recorded on the way to the target when it's this far ahead (the
        // film played there before), instead of playing all of it fast; given up for the film if demo_back doesn't go
        // forward
        constexpr std::int32_t FORWARD_JUMP_MIN_MS = 3000;
        constexpr std::uint64_t FORWARD_JUMP_TIMEOUT_MS = 500;
        bool forwardJumpsWork = true;
        bool jumpingForward = false;
        constexpr std::int32_t SEEK_TOLERANCE_MS = 10;

        std::atomic<std::int32_t> pendingSeek = -1;
        SeekState seekState = SeekState::Idle;
        std::int32_t seekTarget = 0;
        std::uint64_t seekStepStarted = 0;  // when the jump was sent, or when the film last moved while forwarding
        std::int32_t seekLastTime = 0;
        // the timescale forwarding set last, and the film's milliseconds a frame moves at a timescale of 1 (measured
        // as it plays on), so the last step lands on the target's millisecond rather than up to a frame past it
        float seekScale = 1.0f;
        float msPerFrame = 16.0f;
        std::uint64_t lastJumpSent = 0;
        // the last jump went to the film's start (demo_jumptostart), which may be after the target
        bool jumpedToStart = false;
        // the target while a seek runs, for GetSeekTarget (read by the UI thread)
        std::atomic<std::int32_t> activeTarget = -1;
        // what the seek changed, put back at its end
        float savedTimescale = 1.0f;
        bool savedPause = false;

        bool wasPlaying = false;
        // a film that started and isn't told to core yet, while its file is looked for (as long as FILM_START_WAIT_MS)
        bool startPending = false;
        std::uint64_t startPendingSince = 0;
        constexpr std::uint64_t FILM_START_WAIT_MS = 5000;
        // whether core was given the film's length (OnDemoBoundsDetermined)
        bool boundsTold = false;
        // demo_pause as it was last seen, to tell which side changed it
        bool lastGamePause = false;
    }  // namespace

    bool IsFilmPlaying()
    {
        const auto& addresses = GetGameAddresses();
        return ReadGlobal<std::int32_t>(addresses.demoState()) == DEMO_STATE_PLAYING &&
               ReadGlobal<std::uintptr_t>(addresses.theater()) != 0 && GetCg() != 0;
    }

    std::int32_t GetSnapshotTime()
    {
        if (!IsFilmPlaying())
            return 0;
        return ReadGlobal<std::int32_t>(GetCg() + CG_TIME);
    }

    // The frame's time, so the timeline and the dolly camera move every frame, not 10 times a second. It's taken only
    // while it's within a second of the snapshot's, the time everything else here was seen to use.
    std::int32_t GetFilmTime()
    {
        const auto snapshot = GetSnapshotTime();
        if (!snapshot)
            return 0;
        const auto viewCg = ReadGlobal<std::uintptr_t>(GetGameAddresses().cgViewPointer());
        if (!viewCg)
            return snapshot;
        const auto time = ReadGlobal<std::int32_t>(viewCg + CG_VIEW_TIME);
        return std::abs(time - snapshot) <= 1000 ? time : snapshot;
    }

    namespace
    {
        // the offsets in the .demo file of the chunks the theater's snapshots so far start at (the first is the second
        // chunk's, 8 + the size of the first)
        std::vector<std::uint32_t> GetSnapshotFileOffsets()
        {
            std::vector<std::uint32_t> offsets;
            const auto theater = ReadGlobal<std::uintptr_t>(GetGameAddresses().theater());
            if (!theater)
                return offsets;
            const auto count = ReadGlobal<std::int32_t>(theater + SNAPSHOT_COUNT);
            // the first is there from the start, before the count says so
            for (std::int32_t i = 0; i < std::clamp(count, 1, 64); ++i)
            {
                // (an entry not filled yet, or cleared as the film starts again, has 0)
                const auto offset =
                    ReadGlobal<std::uint32_t>(theater + SNAPSHOTS + i * SNAPSHOT_SIZE + SNAPSHOT_FILE_OFFSET);
                if (offset != 0)
                    offsets.push_back(offset);
            }
            return offsets;
        }

        // whether a chunk of the file starts at each offset: walking its chunk headers lands on every one of them
        // (checked on the test film: its snapshots' 5224, 13490, 25851, 50847 and 66278 are all chunk starts)
        bool FileHasChunksAt(const std::filesystem::path& path, const std::vector<std::uint32_t>& offsets)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
                return false;
            file.seekg(0, std::ios::end);
            const auto size = static_cast<std::uint64_t>(file.tellg());

            std::vector<std::uint32_t> wanted(offsets);
            std::sort(wanted.begin(), wanted.end());
            std::size_t next = 0;
            std::uint64_t offset = 0;
            while (next < wanted.size() && offset + 8 <= size)
            {
                if (offset == wanted[next])
                    ++next;
                else if (offset > wanted[next])
                    return false;

                std::uint32_t header[2]{};
                file.seekg(static_cast<std::streamoff>(offset));
                if (!file.read(reinterpret_cast<char*>(header), sizeof(header)) || header[0] == 0)
                    return false;
                offset += 8 + header[0];
            }
            return next == wanted.size();
        }

        // Redacted doesn't give the theater the file's name (its name buffer holds the film's title, "Film"), so the
        // file is the one in the film folder whose chunks start where the theater's snapshots do. With several that
        // fit, the newest.
        // The film's bytes are in the theater's film buffer from the start (0x3205678: the lobby's download, or the
        // Demo tab), so the file is the one in the film folder with the same bytes: at once, and exact even when
        // several films start alike (the snapshots told them apart only as the film played on)
        std::filesystem::path FindFilmFileByBuffer()
        {
            const auto film = GetGameAddresses().filmBuffer();
            const auto data = *reinterpret_cast<const char* const*>(film);
            const auto size = *reinterpret_cast<const std::uint32_t*>(film + 4);
            if (!data || size < 16)
                return {};

            std::error_code error;
            for (const auto& entry : std::filesystem::directory_iterator(GetFilmFolder(), error))
            {
                if (!entry.is_regular_file(error) || _stricmp(entry.path().extension().string().c_str(), ".demo") != 0)
                    continue;
                if (entry.file_size(error) != size)
                    continue;
                std::ifstream file(entry.path(), std::ios::binary);
                std::vector<char> bytes(size);
                if (file.read(bytes.data(), size) && std::memcmp(bytes.data(), data, size) == 0)
                    return entry.path();
            }
            return {};
        }

        std::filesystem::path FindFilmFile()
        {
            if (auto byBuffer = FindFilmFileByBuffer(); !byBuffer.empty())
                return byBuffer;

            const auto offsets = GetSnapshotFileOffsets();
            if (offsets.empty())
                return {};

            std::error_code error;
            std::filesystem::path best;
            std::filesystem::file_time_type bestTime{};
            std::size_t matches = 0;
            for (const auto& entry : std::filesystem::directory_iterator(GetFilmFolder(), error))
            {
                if (!entry.is_regular_file(error) || _stricmp(entry.path().extension().string().c_str(), ".demo") != 0)
                    continue;
                if (!FileHasChunksAt(entry.path(), offsets))
                    continue;
                ++matches;
                const auto time = entry.last_write_time(error);
                if (best.empty() || time > bestTime)
                {
                    best = entry.path();
                    bestTime = time;
                }
            }
            if (matches > 1)
                LOG_WARN("T6: {} films fit the theater's snapshots; taking the newest, {}", matches,
                         best.filename().string());
            return best;
        }
    }  // namespace

    std::string GetFilmTitle()
    {
        const auto buffer = reinterpret_cast<const char*>(GetGameAddresses().filmName());
        std::string name(buffer, strnlen(buffer, FILM_NAME_SIZE));
        if (name.size() > 5 && _stricmp(name.c_str() + name.size() - 5, ".demo") == 0)
            name.resize(name.size() - 5);
        return name;
    }

    std::filesystem::path GetFilmPath()
    {
        if (!IsFilmPlaying())
            return {};
        std::lock_guard lock(fileMutex);
        // Looked for again twice a second until it's found: as the film starts, the theater's snapshots can still be
        // the last film's or half cleared, which no file fits
        const auto now = GetTickCount64();
        if (!filmFileSearched || (filmFile.empty() && now - filmFileSearchedAt >= 500))
        {
            filmFileSearched = true;
            filmFileSearchedAt = now;
            filmFile = FindFilmFile();
            if (!filmFile.empty())
                LOG_INFO("T6: the film's file is {}", filmFile.filename().string());
            else if (!filmFileMissLogged)
            {
                filmFileMissLogged = true;
                LOG_WARN("T6: no film in {} fits the theater's snapshots (yet)", GetFilmFolder().string());
            }
        }
        return filmFile;
    }

    std::string GetFilmName()
    {
        const auto path = GetFilmPath();
        return path.empty() ? GetFilmTitle() : path.stem().string();
    }

    namespace
    {
        // the party session (0x12A7000) and the session mode mask (0x2560890), read out of xpartyplaydemo's code:
        // 6A 05 E8 <Com_SessionMode_IsMode> ... 68 <session>; Com_SessionMode_IsMode is 8B 4C 24 04 B8 01 00 00 00
        // D3 E0 85 05 <mask>
        struct LobbyAddresses
        {
            std::uintptr_t session = 0;
            std::uintptr_t modeMask = 0;
        };

        std::optional<LobbyAddresses> GetLobbyAddresses()
        {
            static const std::optional<LobbyAddresses> lobby = []() -> std::optional<LobbyAddresses> {
                const auto handler = GetGameAddresses().xpartyPlayDemo();
                const auto code = reinterpret_cast<const std::uint8_t*>(handler);
                if (code[0] != 0x6A || code[1] != 0x05 || code[2] != 0xE8 || code[14] != 0x68)
                {
                    LOG_ERROR("T6: xpartyplaydemo at {:#x} isn't as expected", handler);
                    return std::nullopt;
                }
                const auto isMode = handler + 7 + *reinterpret_cast<const std::int32_t*>(handler + 3);
                static constexpr std::uint8_t IS_MODE[] = {0x8B, 0x4C, 0x24, 0x04, 0xB8, 0x01, 0x00,
                                                           0x00, 0x00, 0xD3, 0xE0, 0x85, 0x05};
                if (std::memcmp(reinterpret_cast<const void*>(isMode), IS_MODE, sizeof(IS_MODE)) != 0)
                {
                    LOG_ERROR("T6: Com_SessionMode_IsMode at {:#x} isn't as expected", isMode);
                    return std::nullopt;
                }
                LobbyAddresses result{*reinterpret_cast<const std::uintptr_t*>(handler + 15),
                                      *reinterpret_cast<const std::uintptr_t*>(isMode + 13)};
                LOG_DEBUG("T6: the party session at {:#x}, the session mode at {:#x}", result.session,
                          result.modeMask);
                return result;
            }();
            return lobby;
        }

        // the session's fields that 0x675E90 checks before it sets up the players: +0x9590 1 (running), +0x958C and
        // +0x9584 set (0x6985E0, 0x5C8570, 0x6D2880)
        constexpr std::uintptr_t SESSION_STATE = 0x9590;
        constexpr std::uintptr_t SESSION_HOSTING = 0x958C;
        constexpr std::uintptr_t SESSION_READY = 0x9584;
        // SESSIONMODE_THEATER, the bit xpartyplaydemo tests first
        constexpr std::uint32_t SESSION_MODE_THEATER = 5;
    }  // namespace

    namespace
    {
        // A diagnostic for films started from the Demo tab: for a minute after one, the first few crashing exceptions
        // are logged (where, the registers, the game's return addresses on the stack) before the game's own handler
        // sees them. Nothing is handled here.
        std::atomic<std::uint64_t> crashWatchUntil = 0;
        std::atomic<int> crashesLogged = 0;

        bool IsGameCode(std::uintptr_t address)
        {
            static const auto range = []() {
                const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
                const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
                const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
                return std::pair(base, base + nt->OptionalHeader.SizeOfImage);
            }();
            return address >= range.first && address < range.second;
        }

        std::string DescribeAddress(std::uintptr_t address)
        {
            HMODULE module = nullptr;
            char name[MAX_PATH]{};
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(address), &module) &&
                GetModuleFileNameA(module, name, MAX_PATH))
                return std::format("{:#x} ({}+{:#x})", address, std::filesystem::path(name).filename().string(),
                                   address - reinterpret_cast<std::uintptr_t>(module));
            return std::format("{:#x}", address);
        }

        LONG CALLBACK LogCrash(EXCEPTION_POINTERS* info)
        {
            const auto code = info->ExceptionRecord->ExceptionCode;
            const bool crash = code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
                               code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_STACK_OVERFLOW ||
                               code == EXCEPTION_PRIV_INSTRUCTION || code == 0xC0000374;  // STATUS_HEAP_CORRUPTION
            if (!crash || GetTickCount64() > crashWatchUntil || crashesLogged.fetch_add(1) >= 4)
                return EXCEPTION_CONTINUE_SEARCH;

            const auto* c = info->ContextRecord;
            std::string line = std::format("T6: exception {:#x} at {}", static_cast<std::uint32_t>(code),
                                           DescribeAddress(reinterpret_cast<std::uintptr_t>(
                                               info->ExceptionRecord->ExceptionAddress)));
            if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2)
                line += std::format(" ({} {:#x})", info->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                                    info->ExceptionRecord->ExceptionInformation[1]);
            line += std::format("; eax={:#x} ebx={:#x} ecx={:#x} edx={:#x} esi={:#x} edi={:#x} ebp={:#x} esp={:#x}",
                                c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
            LOG_ERROR("{}", line);

            // the game's code addresses on the stack, most of them return addresses
            std::string stack;
            int found = 0;
            for (std::uintptr_t p = c->Esp; p < c->Esp + 0x1000 && found < 24; p += 4)
            {
                std::uintptr_t value = 0;
                if (IsBadReadPtr(reinterpret_cast<const void*>(p), 4))
                    break;
                value = *reinterpret_cast<const std::uintptr_t*>(p);
                if (IsGameCode(value))
                {
                    stack += std::format(" {:#x}", value);
                    ++found;
                }
            }
            LOG_ERROR("T6: the game's addresses on the stack:{}", stack);

            // the address space left: a 32-bit game short of it fails its allocations, and goes on with null pointers
            std::uint64_t freeBytes = 0;
            std::uint64_t largest = 0;
            MEMORY_BASIC_INFORMATION region{};
            for (std::uintptr_t address = 0x10000;
                 VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region)) == sizeof(region);)
            {
                if (region.State == MEM_FREE)
                {
                    freeBytes += region.RegionSize;
                    largest = std::max<std::uint64_t>(largest, region.RegionSize);
                }
                const auto next = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
                if (next <= address)
                    break;
                address = next;
            }
            PROCESS_MEMORY_COUNTERS_EX counters{};
            GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                 sizeof(counters));
            LOG_ERROR("T6: address space free {} MB (largest block {} MB), the game's own memory {} MB",
                      freeBytes >> 20, largest >> 20, static_cast<std::uint64_t>(counters.PrivateUsage) >> 20);
            return EXCEPTION_CONTINUE_SEARCH;
        }

        void WatchForCrash()
        {
            static const auto handler = AddVectoredExceptionHandler(1, LogCrash);
            (void)handler;
            crashesLogged = 0;
            crashWatchUntil = GetTickCount64() + 60000;
        }

        // the lobby's chosen film (SetTheaterFileInfo, 0x977330, copies it to 0x3208198 through 0x585130)
        void LogLobbyFilm()
        {
            const auto text = [](std::uintptr_t address, std::size_t size) {
                const auto chars = reinterpret_cast<const char*>(address);
                return std::string(chars, strnlen(chars, size));
            };
            LOG_DEBUG("T6: the lobby's film: name '{}', description '{}', category '{}', duration {}, pooled {}, "
                      "match {:#x}, file {:#x}; ui_demoname was '{}'",
                      text(0x3208198, 0x40), text(0x32081D8, 0x40), text(0x3208218, 0x20),
                      ReadGlobal<std::int32_t>(0x3208238), ReadGlobal<std::uint8_t>(0x320823C),
                      ReadGlobal<std::uint64_t>(0x3208240), ReadGlobal<std::uint64_t>(0x3208248),
                      [] {
                          const auto dvar = Functions::FindDvar("ui_demoname");
                          return dvar && dvar->current.string ? std::string(dvar->current.string) : std::string();
                      }());
        }
    }  // namespace

    namespace
    {
        // The party of a lobby searches for players (0x6AA860, every party_searchInterval) while it has fewer than
        // party_minplayers, and merges with others once it has party_dedicatedMergeMinPlayers (party_minplayers when
        // that's 0). The lobby the Demo tab opens did both: the search crashed in the LUI's missing
        // SetupMatchmakingQuery (0x6E6A61), and with party_minplayers 1 the merge restarted the client game, which
        // crashed as it began again (0x626C88, 9 s into the film). So neither: a minimum of 1 and a merge at 32.
        // a film the Demo tab started, whose party is kept from searching while it plays (see OnFrame)
        std::atomic<bool> editorFilm = false;

        // The theater records its snapshots (the points a film can go back to, 0x92C680) only while a match may be
        // recorded too (0x591C10): the session's modes with the online game (bit 2 of 0x2562AC4) and demo_enabled.
        // The game's own Theater button has them; the lobby the Demo tab opened from the main menu hadn't the online
        // game, so its films couldn't go back at all. So the Demo tab sets them, as the LUI's SessionModeSetOnlineGame
        // does.
        constexpr std::uint32_t SESSION_MODE_ONLINE = 2;
        void AllowSnapshots()
        {
            const auto modes = reinterpret_cast<volatile std::uint32_t*>(GetGameAddresses().sessionModes());
            const auto before = *modes;
            *modes = before | (1u << SESSION_MODE_ONLINE);
            Functions::SetDvarNumber("demo_enabled", 1.0f);
            if (before != *modes)
                LOG_INFO("T6: the session's modes {:#x} -> {:#x} (the online game), so the theater records snapshots",
                         before, static_cast<std::uint32_t>(*modes));
        }

        void KeepPartyFromSearching()
        {
            Functions::SetDvarNumber("party_minplayers", 1.0f);
            Functions::SetDvarNumber("party_dedicatedMergeMinPlayers", 32.0f);
        }

        bool InGame()
        {
            const auto cl_ingame = Functions::FindDvar("cl_ingame");
            return cl_ingame && cl_ingame->type == Structures::DVAR_TYPE_BOOL && cl_ingame->current.enabled;
        }

        // the party session up as a theater lobby: what xpartyplaydemo and 0x675E90 check
        bool IsTheaterLobbyUp()
        {
            const auto lobby = GetLobbyAddresses();
            if (!lobby || InGame() || IsFilmPlaying())
                return false;
            const auto session = lobby->session;
            return ReadGlobal<std::int32_t>(session + SESSION_STATE) == 1 &&
                   ReadGlobal<std::int32_t>(session + SESSION_HOSTING) != 0 &&
                   ReadGlobal<std::int32_t>(session + SESSION_READY) != 0;
        }

        // A film asked for from the main menu: the Theater's lobby is opened first (setactivemenu theater, 0x8A3CD0,
        // which takes the lobby to its TheaterLobby, 0x49BAA0 case 21), and the film starts once the lobby has been up
        // for LOBBY_SETTLE_MS, or not at all after LOBBY_WAIT_MS.
        struct PendingFilm
        {
            std::filesystem::path file;
            std::uint64_t since = 0;
            std::uint64_t upSince = 0;
            // the lobby's steps sent so far: the party hosted (xstartpartyhost), the theater menu again after it
            bool hostSent = false;
            bool menuAgainSent = false;
        };
        // milliseconds the lobby gets to host its party by itself before xstartpartyhost is sent
        constexpr std::uint64_t HOST_AFTER_MS = 2000;
        std::optional<PendingFilm> pendingFilm;

        // xpartygo, but not for the lobby the Demo tab opened: its party started a match there (the map loaded over
        // the film and the game crashed, 0x626C88, 9 s into it)
        typedef void(__cdecl* XPartyGo_t)();
        XPartyGo_t XPartyGo_Trampoline = nullptr;
        void __cdecl XPartyGo_Hook()
        {
            if (editorFilm || pendingFilm)
            {
                LOG_INFO("T6: not starting the party's match (xpartygo) in the Theater's lobby the editor opened");
                return;
            }
            XPartyGo_Trampoline();
        }

        constexpr std::uint64_t LOBBY_SETTLE_MS = 1500;
        constexpr std::uint64_t LOBBY_WAIT_MS = 30000;

        std::string StartFromLobby(const std::filesystem::path& file);

        void UpdatePendingFilm()
        {
            if (!pendingFilm)
                return;
            const auto now = GetTickCount64();
            // (what the lobby's fields are as it opens, each time they change)
            if (const auto lobby = GetLobbyAddresses())
            {
                const auto session = lobby->session;
                const auto seen = std::format("session state {} hosting {} ready {}, session modes {:#x}, in game {}",
                                              ReadGlobal<std::int32_t>(session + SESSION_STATE),
                                              ReadGlobal<std::int32_t>(session + SESSION_HOSTING),
                                              ReadGlobal<std::int32_t>(session + SESSION_READY),
                                              ReadGlobal<std::uint32_t>(lobby->modeMask), InGame());
                static std::string lastSeen;
                if (seen != lastSeen)
                {
                    lastSeen = seen;
                    LOG_DEBUG("T6: opening the Theater's lobby ({} ms): {}", now - pendingFilm->since, seen);
                }
            }
            const auto lobbyAddresses = GetLobbyAddresses();
            const bool theaterMode =
                lobbyAddresses &&
                (ReadGlobal<std::uint32_t>(lobbyAddresses->modeMask) & (1u << SESSION_MODE_THEATER)) != 0;
            if (!IsTheaterLobbyUp() || !theaterMode)
            {
                // setactivemenu theater opens the lobby's menu and its session (state 1, the theater's mode), but
                // its party isn't hosted (+0x958C, +0x9584 stay 0) as it is from the game's Theater button: so the
                // party is hosted as xstartpartyhost does (0x6BA0B0 on the party session), and the theater's menu
                // asked for again if that took the theater's mode away
                if (const auto lobby = GetLobbyAddresses())
                {
                    const auto session = lobby->session;
                    const bool hosted = ReadGlobal<std::int32_t>(session + SESSION_HOSTING) != 0;
                    const bool theater =
                        (ReadGlobal<std::uint32_t>(lobby->modeMask) & (1u << SESSION_MODE_THEATER)) != 0;
                    if (!hosted && !pendingFilm->hostSent && now - pendingFilm->since >= HOST_AFTER_MS)
                    {
                        KeepPartyFromSearching();
                        AllowSnapshots();
                        pendingFilm->hostSent = true;
                        LOG_INFO("T6: hosting the Theater's party");
                        Functions::Cbuf_AddText("xstartpartyhost");
                    }
                    else if (hosted && !theater && pendingFilm->hostSent && !pendingFilm->menuAgainSent)
                    {
                        pendingFilm->menuAgainSent = true;
                        LOG_INFO("T6: the party is hosted without the theater's mode; opening the Theater's menu again");
                        Functions::Cbuf_AddText("setactivemenu theater");
                    }
                }
                pendingFilm->upSince = 0;
                if (now - pendingFilm->since >= LOBBY_WAIT_MS)
                {
                    LOG_WARN("T6: the Theater's lobby didn't open within {} s; not playing {}", LOBBY_WAIT_MS / 1000,
                             pendingFilm->file.filename().string());
                    pendingFilm.reset();
                }
                return;
            }
            if (!pendingFilm->upSince)
            {
                pendingFilm->upSince = now;
                LOG_DEBUG("T6: the Theater's lobby is up, {} ms after it was opened", now - pendingFilm->since);
            }
            if (now - pendingFilm->upSince < LOBBY_SETTLE_MS)
                return;
            const auto file = std::move(pendingFilm->file);
            pendingFilm.reset();
            if (const auto error = StartFromLobby(file); !error.empty())
                LOG_WARN("T6: not playing {}: {}", file.filename().string(), error);
        }
    }  // namespace

    void InstallHooks()
    {
        HookManager::CreateHook(GetGameAddresses().xpartyGo(), reinterpret_cast<std::uintptr_t>(XPartyGo_Hook),
                                reinterpret_cast<std::uintptr_t*>(&XPartyGo_Trampoline));
        HookManager::CreateHook(GetGameAddresses().partyAutoStart(),
                                reinterpret_cast<std::uintptr_t>(&PartyAutoStart_Hook), &t6irisAutoStartTrampoline);
    }

    std::string PlayFilmFromLobby(const std::filesystem::path& file)
    {
        if (pendingFilm)
            return "A film is waiting for the Theater's lobby to open.";
        if (IsFilmPlaying())
            return "A film is playing. Leave it for the Theater's lobby (Escape, then End Film), then play this one "
                   "from here.";
        if (!GetLobbyAddresses())
            return "This game's Theater code isn't the one T6iris knows, so films open from the Theater menu.";
        if (IsTheaterLobbyUp())
            return StartFromLobby(file);
        if (InGame())
            return "Leave this game first, then play the film from the main menu or the Theater's lobby.";

        const auto name = file.stem().string();
        if (name.empty() || name.find_first_of(" \";") != std::string::npos)
            return "The film's file name has a space, quote or semicolon, which the game's demo_play can't take. "
                   "Rename the file in Plugins\\OpenNetStorage\\Cloud\\FILM_MP.";
        LOG_INFO("T6: opening the Theater's lobby for {}", name);
        // The party of a lobby with fewer players than party_minplayers searches for more every party_searchInterval
        // (0x6AA860), which calls the LUI's SetupMatchmakingQuery: the Theater's menus haven't one, and the game crashed
        // reporting it (0x6E6A61, 12 to 16 s after PLAY). The game's own Theater lobby has a minimum of 1, as here.
        KeepPartyFromSearching();
        AllowSnapshots();
        Functions::Cbuf_AddText("setactivemenu theater");
        pendingFilm = PendingFilm{file, GetTickCount64()};
        return {};
    }

    namespace
    {
    std::string StartFromLobby(const std::filesystem::path& file)
    {
        const auto name = file.stem().string();
        if (IsFilmPlaying())
            return "A film is playing. Leave it for the Theater's lobby (Escape, then End Film), then play this one "
                   "from here.";

        const auto lobby = GetLobbyAddresses();
        if (!lobby)
            return "This game's Theater code isn't the one T6iris knows, so films open from the Theater menu.";

        const auto session = lobby->session;
        const auto state = ReadGlobal<std::int32_t>(session + SESSION_STATE);
        const auto hosting = ReadGlobal<std::int32_t>(session + SESSION_HOSTING);
        const auto ready = ReadGlobal<std::int32_t>(session + SESSION_READY);
        const auto modes = ReadGlobal<std::uint32_t>(lobby->modeMask);
        LOG_DEBUG("T6: playing {}: session state {} hosting {} ready {}, session modes {:#x}", name, state, hosting,
                  ready, modes);

        const auto cl_ingame = Functions::FindDvar("cl_ingame");
        const bool inGame = cl_ingame && cl_ingame->type == Structures::DVAR_TYPE_BOOL && cl_ingame->current.enabled;
        if (state != 1 || hosting == 0 || ready == 0 || inGame)
            return "The Theater's lobby isn't ready yet; try again in a moment.";
        if ((modes & (1u << SESSION_MODE_THEATER)) == 0)
            LOG_WARN("T6: the session mode isn't the theater's ({:#x}); playing {} from this lobby anyway", modes, name);

        if (name.empty() || name.find_first_of(" \";") != std::string::npos)
            return "The film's file name has a space, quote or semicolon, which the game's demo_play can't take. "
                   "Rename the file in Plugins\\OpenNetStorage\\Cloud\\FILM_MP.";

        // The theater plays the film from memory (the buffer the lobby's download fills), not from the file: without
        // a film downloaded it read from null and crashed, and after one it played that one. So the file goes in
        // the buffer as the download would leave it: VirtualAlloc'd (the game frees it with VirtualFree), its size,
        // the read position 0.
        std::vector<char> bytes;
        {
            std::ifstream in(file, std::ios::binary);
            if (in)
                bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        if (bytes.empty() || bytes.size() > 0x40000000)
            return std::format("Couldn't read {}.", file.filename().string());
        const auto buffer = VirtualAlloc(nullptr, bytes.size(), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!buffer)
            return "Couldn't get the memory for the film.";
        std::memcpy(buffer, bytes.data(), bytes.size());

        const auto film = GetGameAddresses().filmBuffer();
        auto& data = *reinterpret_cast<void* volatile*>(film);
        auto& size = *reinterpret_cast<volatile std::uint32_t*>(film + 4);
        auto& position = *reinterpret_cast<volatile std::uint32_t*>(film + 8);
        void* const previous = data;
        data = buffer;
        size = static_cast<std::uint32_t>(bytes.size());
        position = 0;
        if (previous)
            VirtualFree(previous, 0, MEM_RELEASE);

        // (see PlayFilmFromLobby: no search for more players during the film)
        KeepPartyFromSearching();
        AllowSnapshots();
        editorFilm = true;
        LOG_INFO("T6: playing {} from the Theater's lobby ({} bytes in the theater's film buffer at {:#x})", name,
                 bytes.size(), film);
        LogLobbyFilm();
        WatchForCrash();
        Functions::Cbuf_AddText(std::format("set ui_demoname {}", name));
        Functions::Cbuf_AddText("xpartyplaydemo");
        return {};
    }
    }  // namespace

    std::optional<FilmTimes> ReadFilmTimes(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return std::nullopt;
        const std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        // chunks of u32 compressed size, u32 size, then a bare deflate stream; the last whole one is the metadata
        std::size_t offset = 0;
        std::optional<std::pair<std::size_t, std::uint32_t>> last;
        while (offset + 8 <= data.size())
        {
            std::uint32_t compressed = 0, size = 0;
            std::memcpy(&compressed, data.data() + offset, 4);
            std::memcpy(&size, data.data() + offset + 4, 4);
            if (compressed == 0 || offset + 8 + compressed > data.size())
                break;
            last = {offset + 8, compressed};
            offset += 8 + compressed;
        }
        if (!last)
            return std::nullopt;

        std::uint32_t size = 0;
        std::memcpy(&size, data.data() + last->first - 4, 4);
        const auto metadata = Inflate::Raw(std::span(data.data() + last->first, last->second), size);
        if (!metadata || metadata->size() < 12)
            return std::nullopt;

        std::int32_t start = 0, end = 0;
        std::memcpy(&start, metadata->data() + 4, 4);
        std::memcpy(&end, metadata->data() + 8, 4);
        if (start < 0 || end <= start)
            return std::nullopt;
        return FilmTimes{start, end};
    }

    std::optional<FilmTimes> GetFilmTimes()
    {
        const auto path = GetFilmPath();
        std::lock_guard lock(timesMutex);
        if (path != timesPath)
        {
            timesPath = path;
            times = path.empty() ? std::nullopt : ReadFilmTimes(path);
            if (times)
                LOG_INFO("T6: film {} runs from {} to {} ms", path.filename().string(), times->start, times->end);
            else if (!path.empty())
                LOG_WARN("T6: couldn't read the start and end of {}", path.string());
        }
        return times;
    }

    namespace
    {
        // Dragging the timeline asks for one seek after another: a second one within SCRUB_GAP_MS of the last makes
        // it a drag until SCRUB_GAP_MS after the last, during which the film is shown as it goes (IsScrubbing)
        constexpr std::uint64_t SCRUB_GAP_MS = 400;
        // while dragged, how far before the film's time a seek is before it goes back
        constexpr std::int32_t SCRUB_BACK_MS = 60;
        std::atomic<std::uint64_t> lastSeekAskedAt = 0;
        std::atomic<std::int32_t> lastSeekAsked = -1;
        std::atomic<std::uint64_t> scrubUntil = 0;
    }  // namespace

    void SeekTo(std::int32_t filmTime)
    {
        if (filmTime != pendingSeek.load())
            LOG_DEBUG("T6: seek asked for {} (film at {})", filmTime, GetFilmTime());
        const auto now = GetTickCount64();
        if (filmTime != lastSeekAsked.exchange(filmTime))
        {
            if (now - lastSeekAskedAt.load() < SCRUB_GAP_MS)
                scrubUntil = now + SCRUB_GAP_MS;
            lastSeekAskedAt = now;
        }
        pendingSeek = std::max(filmTime, 0);
    }

    bool IsScrubbing()
    {
        return IsSeeking() && GetTickCount64() < scrubUntil.load();
    }

    namespace
    {
        constexpr std::uintptr_t THEATER_HUD_HIDDEN = 0x63569;
        constexpr std::uintptr_t THEATER_CONTROLS_HIDDEN = 0x6356A;

        std::atomic<bool> hudShown = false;
        // the theater's own flags from before the editor took them, with the structure they're in
        struct SavedHud
        {
            std::uintptr_t theater;
            std::uint8_t hud;
            std::uint8_t controls;
        };
        std::optional<SavedHud> savedHud;

        void UpdateHud(bool playing)
        {
            const auto theater = playing ? ReadGlobal<std::uintptr_t>(GetGameAddresses().theater()) : 0;
            if (savedHud && savedHud->theater != theater)
                savedHud.reset();
            if (!theater)
                return;

            auto& hud = *reinterpret_cast<volatile std::uint8_t*>(theater + THEATER_HUD_HIDDEN);
            auto& controls = *reinterpret_cast<volatile std::uint8_t*>(theater + THEATER_CONTROLS_HIDDEN);
            const bool editorHasIt =
                !UI::UIManager::Get().IsOverlayHidden() || Components::CaptureManager::Get().IsCapturing();
            if (editorHasIt)
            {
                if (!savedHud)
                    savedHud = SavedHud{theater, hud, controls};
                hud = hudShown ? 0 : 1;
                controls = 1;
            }
            else if (savedHud)
            {
                hud = savedHud->hud;
                controls = savedHud->controls;
                savedHud.reset();
            }
        }
    }  // namespace

    void SetHudShown(bool shown)
    {
        hudShown = shown;
    }

    bool IsSeeking()
    {
        return pendingSeek.load() >= 0 || seekState != SeekState::Idle;
    }

    std::optional<std::int32_t> GetSeekTarget()
    {
        const auto pending = pendingSeek.load();
        if (pending >= 0)
            return pending;
        const auto active = activeTarget.load();
        return active >= 0 ? std::optional<std::int32_t>(active) : std::nullopt;
    }

    namespace
    {
        // the time of the earliest snapshot demo_back goes to (the second; entries 0 to the count are filled),
        // none before the theater has recorded it
        // The snapshots' times, as the theater has them: a ring of 250 (0x92C680 writes the next at +0x63490, the
        // count modulo 250, and marks the ones whose part of the film's stream was written over with the time -1),
        // so the valid ones can be anywhere in it, in time order or not
        constexpr std::size_t SNAPSHOT_RING = 250;
        std::vector<std::int32_t> GetSnapshotTimes()
        {
            std::vector<std::int32_t> times;
            const auto theater = ReadGlobal<std::uintptr_t>(GetGameAddresses().theater());
            if (!theater)
                return times;
            for (std::size_t i = 0; i < SNAPSHOT_RING; ++i)
            {
                const auto time = ReadGlobal<std::int32_t>(theater + SNAPSHOTS + i * SNAPSHOT_SIZE + SNAPSHOT_TIME);
                if (time > 0)
                    times.push_back(time);
            }
            std::sort(times.begin(), times.end());
            return times;
        }

        // the time of the earliest snapshot demo_back goes to (not the film's first: demo_back to it does nothing),
        // none before the theater has recorded one
        std::optional<std::int32_t> GetFirstJumpableSnapshotTime()
        {
            const auto times = GetSnapshotTimes();
            if (times.size() < 2)
                return std::nullopt;
            return times[1];
        }

        Structures::dvar_t* FindBool(const char* name)
        {
            const auto dvar = Functions::FindDvar(name);
            return dvar && dvar->type == Structures::DVAR_TYPE_BOOL ? dvar : nullptr;
        }

        Structures::dvar_t* FindFloat(const char* name)
        {
            const auto dvar = Functions::FindDvar(name);
            return dvar && dvar->type == Structures::DVAR_TYPE_FLOAT ? dvar : nullptr;
        }

        void SetPause(bool paused)
        {
            if (const auto demo_pause = FindBool("demo_pause"); demo_pause && demo_pause->current.enabled != paused)
            {
                demo_pause->current.enabled = paused;
                demo_pause->modified = true;
            }
        }

        void SetTimescale(float value)
        {
            if (const auto timescale = FindFloat("timescale"); timescale && timescale->current.value != value)
            {
                timescale->current.value = value;
                timescale->modified = true;
            }
        }

        void FinishSeek(const char* why)
        {
            SetTimescale(savedTimescale);
            SetPause(savedPause);
            lastGamePause = savedPause;
            LOG_DEBUG("T6: seek to {} ended at {} ({})", seekTarget, GetFilmTime(), why);
            jumpingForward = false;
            seekState = SeekState::Idle;
            activeTarget = -1;
        }

        // Runs the seek once a frame; true while one is under way (and the pause isn't to be synced)
        bool UpdateSeek()
        {
            const auto now = GetTickCount64();
            const auto time = GetFilmTime();

            const auto requested = pendingSeek.load();
            if (requested >= 0)
            {
                // a seek that goes back can only be sent every so often (dragging asks every frame); one that goes
                // forward just moves the target
                // (any time before the film's for a seek on its own: a node's or a key's exact millisecond is asked
                // for; while the timeline is dragged, only a time well before it, or each frame's few milliseconds
                // past the drag would send the film back, to its start when that's before its first snapshot)
                const bool dragged = now < scrubUntil.load();
                const bool back = requested < time - (dragged ? SCRUB_BACK_MS : 0);
                if (!back || now - lastJumpSent >= SEEK_INTERVAL_MS)
                {
                    pendingSeek = -1;
                    if (seekState == SeekState::Idle)
                    {
                        const auto timescale = FindFloat("timescale");
                        savedTimescale = timescale ? timescale->current.value : 1.0f;
                        savedPause = Components::Playback::IsPaused();
                    }

                    std::int32_t target = requested;
                    if (const auto times = GetFilmTimes())
                        target = std::clamp(target, times->start, times->end - 100);
                    seekTarget = target;
                    activeTarget = target;

                    if (back)
                    {
                        // before the second snapshot, the film's start; forwarding goes on from there
                        const auto second = GetFirstJumpableSnapshotTime();
                        jumpedToStart = !second || target < *second;
                        {
                            const auto snapshots = GetSnapshotTimes();
                            std::string list;
                            for (std::size_t i = 0; i < snapshots.size() && i < 24; ++i)
                                list += std::format(" {}", snapshots[i]);
                            const auto theater = ReadGlobal<std::uintptr_t>(GetGameAddresses().theater());
                            LOG_DEBUG("T6: going back to {} from {} by {}; the theater's {} snapshots (next {}):{}", target,
                                      time, jumpedToStart ? "demo_jumptostart" : "demo_back", snapshots.size(),
                                      theater ? ReadGlobal<std::int32_t>(theater + SNAPSHOT_COUNT) : -1, list);
                        }
                        if (jumpedToStart)
                            Functions::Cbuf_AddText("demo_jumptostart");
                        else
                            Functions::Cbuf_AddText(std::format("demo_back {}", target));
                        lastJumpSent = now;
                        retriedJump = false;
                        jumpingForward = false;
                        seekState = SeekState::Jumping;
                        seekStepStarted = now;
                        seekLastTime = time;
                    }
                    else if (seekState != SeekState::Jumping)
                    {
                        jumpedToStart = false;
                        // the last snapshot at or before the target, when it's well ahead: demo_back goes to it
                        std::optional<std::int32_t> ahead;
                        if (forwardJumpsWork)
                        {
                            for (const auto snapshot : GetSnapshotTimes())
                                if (snapshot <= target && snapshot > time + FORWARD_JUMP_MIN_MS)
                                    ahead = snapshot;
                        }
                        if (ahead)
                        {
                            LOG_DEBUG("T6: jumping forward to the snapshot at {} for {}", *ahead, target);
                            Functions::Cbuf_AddText(std::format("demo_back {}", target));
                            lastJumpSent = now;
                            retriedJump = true;
                            jumpingForward = true;
                            seekState = SeekState::Jumping;
                        }
                        else
                        {
                            seekState = SeekState::Forwarding;
                        }
                        seekStepStarted = now;
                        seekLastTime = time;
                    }
                }
            }

            switch (seekState)
            {
                case SeekState::Idle:
                    return false;

                case SeekState::Jumping:
                    // a jump forward that went, or didn't (then the film plays on to the target, and no more of them)
                    if (jumpingForward)
                    {
                        const bool went = time > seekLastTime + FORWARD_JUMP_MIN_MS / 2;
                        if (went || now - seekStepStarted >= FORWARD_JUMP_TIMEOUT_MS)
                        {
                            if (!went)
                            {
                                forwardJumpsWork = false;
                                LOG_WARN("T6: demo_back didn't jump forward (at {}); seeks forward play on instead",
                                         time);
                            }
                            jumpingForward = false;
                            seekState = SeekState::Forwarding;
                            seekStepStarted = now;
                            seekLastTime = time;
                        }
                        SetPause(false);
                        seekScale = 1.0f;
                        SetTimescale(1.0f);
                        return true;
                    }
                    // the theater went back (to the snapshot), or it didn't and the film is on from here
                    if (time < seekLastTime - SEEK_TOLERANCE_MS || now - seekStepStarted >= JUMP_TIMEOUT_MS)
                    {
                        seekState = SeekState::Forwarding;
                        seekStepStarted = now;
                        seekLastTime = time;
                    }
                    SetPause(false);
                    seekScale = 1.0f;
                    SetTimescale(1.0f);
                    return true;

                case SeekState::Forwarding:
                {
                    const auto remaining = seekTarget - time;
                    // the start is where the film begins playing (~0.6 s), which can be after a target near 0
                    if (jumpedToStart && remaining < 0 && time < 2000)
                    {
                        FinishSeek("the film's start");
                        return false;
                    }
                    if (remaining < -SEEK_TOLERANCE_MS * 10 && !retriedJump)
                    {
                        // once more, by the other command: demo_back for a demo_jumptostart that didn't go back, and
                        // the other way round
                        retriedJump = true;
                        const bool toStart = !jumpedToStart;
                        LOG_WARN("T6: the theater didn't go back to {} (at {}); trying {}", seekTarget, time,
                                 toStart ? "demo_jumptostart" : "demo_back");
                        jumpedToStart = toStart;
                        Functions::Cbuf_AddText(toStart ? std::string("demo_jumptostart")
                                                        : std::format("demo_back {}", seekTarget));
                        lastJumpSent = now;
                        seekState = SeekState::Jumping;
                        seekStepStarted = now;
                        seekLastTime = time;
                        return true;
                    }
                    if (remaining < -SEEK_TOLERANCE_MS * 10)
                    {
                        LOG_WARN("T6: couldn't go back to {}: the film is still at {}", seekTarget, time);
                        FinishSeek("the theater didn't go back");
                        return false;
                    }
                    if (remaining <= 0)
                    {
                        FinishSeek(remaining == 0 ? "there" : "just past");
                        return false;
                    }
                    if (time != seekLastTime)
                    {
                        if (time > seekLastTime && seekScale > 0.0f)
                            msPerFrame = std::clamp(static_cast<float>(time - seekLastTime) / seekScale, 1.0f, 100.0f);
                        seekLastTime = time;
                        seekStepStarted = now;
                    }
                    else if (now - seekStepStarted >= STALL_TIMEOUT_MS)
                    {
                        FinishSeek("the film stopped moving");
                        return false;
                    }

                    // fast while far, then down to normal speed for the last SLOWDOWN_MS of film, and the last frame
                    // slowed to land on the target (a millisecond a frame at least, so the film still moves)
                    SetPause(false);
                    seekScale = std::clamp(static_cast<float>(remaining) / SLOWDOWN_MS, 1.0f, MAX_SEEK_SPEED);
                    if (msPerFrame * seekScale > static_cast<float>(remaining))
                        seekScale = static_cast<float>(remaining) / msPerFrame;
                    SetTimescale(seekScale);
                    return true;
                }
            }
            return false;
        }
        // whether fixedtime is the capture's now, to put it back to 0 once
        bool recordingTime = false;

        // Records' time: see OnFrame in Theater.hpp. Returns whether the capture has the film's time and pause now.
        bool UpdateRecording(bool playing, bool seeking)
        {
            auto& capture = Components::CaptureManager::Get();
            const bool recording = playing && !seeking && capture.IsCapturing();
            const auto fixedtime = Functions::FindDvar("fixedtime");
            if (!fixedtime || fixedtime->type != Structures::DVAR_TYPE_INT)
            {
                if (recording && !recordingTime)
                    LOG_ERROR("T6: no fixedtime dvar, so the recording plays at the game's own speed");
                recordingTime = recording;
                return recording;
            }

            if (recording)
            {
                const auto framerate = std::max(capture.GetCaptureSettings().framerate, 1);
                const auto frameTime = std::clamp(1000 / framerate, 1, 1000);
                if (!recordingTime)
                    LOG_INFO("T6: recording, {} ms of the film a frame", frameTime);
                // (a multi-pass capture draws each recorded frame's passes, with their settle frames, at one moment
                // of the film: 1 ms each, fixedtime's least (0 is the game's own time), and the frame that moves the
                // film on that much less, so a recorded frame is still a frame's time)
                const auto setFrames = capture.GetPassSetFrames();
                if (setFrames > 1)
                    fixedtime->current.integer =
                        capture.PassTimeAdvances() ? std::max(frameTime - (setFrames - 1), 1) : 1;
                else
                    fixedtime->current.integer = frameTime;
                SetPause(false);
                lastGamePause = false;
            }
            else if (recordingTime)
            {
                fixedtime->current.integer = 0;
                LOG_INFO("T6: the film's time is the game's again");
            }
            recordingTime = recording;
            return recording;
        }
    }  // namespace

    void OnFrame()
    {
        // (a diagnostic: the lobby's film each time ui_demoname changes, as the Theater's Select Film sets it)
        {
            static std::string lastDemoName;
            static std::uint64_t lastCheck = 0;
            if (const auto now = GetTickCount64(); now - lastCheck >= 500)
            {
                lastCheck = now;
                const auto dvar = Functions::FindDvar("ui_demoname");
                const std::string value = dvar && dvar->current.string ? dvar->current.string : "";
                if (value != lastDemoName)
                {
                    lastDemoName = value;
                    LOG_DEBUG("T6: ui_demoname is '{}'", value);
                    LogLobbyFilm();
                }
            }
        }

        UpdatePendingFilm();

        const bool playing = IsFilmPlaying();
        // the lobby the editor opened doesn't count down to a match (see PartyAutoStart_Hook)
        {
            const bool hold = editorFilm || pendingFilm.has_value();
            if (hold != (t6irisHoldAutoStart != 0))
            {
                t6irisHoldAutoStart = hold ? 1 : 0;
                LOG_INFO("T6: the party's auto start {}", hold ? "held for the editor's lobby" : "let go");
            }
        }

        // (the lobby's menus can set the party's minimum again; once a second while the Demo tab's film plays)
        if (editorFilm)
        {
            static std::uint64_t lastKept = 0;
            if (GetTickCount64() - lastKept >= 1000)
            {
                lastKept = GetTickCount64();
                KeepPartyFromSearching();
                AllowSnapshots();
            }
            // (until the lobby is left: its party could start a match after the film too)
            static std::uint64_t goneSince = 0;
            if (!playing && !pendingFilm && !IsTheaterLobbyUp())
            {
                if (!goneSince)
                    goneSince = GetTickCount64();
                else if (GetTickCount64() - goneSince > 5000)
                {
                    editorFilm = false;
                    goneSince = 0;
                }
            }
            else
            {
                goneSince = 0;
            }
        }
        if (playing != wasPlaying)
        {
            wasPlaying = playing;
            if (playing)
            {
                forwardJumpsWork = true;
                {
                    std::lock_guard lock(fileMutex);
                    filmFileSearched = false;
                    filmFileMissLogged = false;
                    filmFile.clear();
                }
                startPending = true;
                startPendingSince = GetTickCount64();
            }
            else
            {
                startPending = false;
                LOG_INFO("T6: film stopped");
            }
        }

        // Core is told of the film once its file is known (the keyframes are kept by its name, and the timeline's view
        // and the capture range come from its length), or after FILM_START_WAIT_MS without it
        if (playing && startPending)
        {
            const bool found = !GetFilmPath().empty();
            if (!found && GetTickCount64() - startPendingSince < FILM_START_WAIT_MS)
                return;
            startPending = false;
            {
                LOG_INFO("T6: film '{}' started (file: {}; the session's modes {:#x})", GetFilmTitle(),
                         GetFilmPath().string(), ReadGlobal<std::uint32_t>(GetGameAddresses().sessionModes()));
                // (a crash in the film's first minute is logged too, however it was started)
                WatchForCrash();
                // the theater has loaded it by now, so both at once
                Events::Invoke(EventType::PreDemoLoad);
                Events::Invoke(EventType::PostDemoLoad);
                Lights::OnFilmStarted();
                Props::OnFilmStarted();
                // The timeline sets its view (what a click's x is a tick of) and the capture range their default from
                // the demo's length, on this event; T5's demo parser raises it. Without it the view stays 0 to 0 and
                // every click on the timeline is tick 0. The length is read from the file first.
                boundsTold = GetFilmTimes().has_value();
                if (boundsTold)
                    Events::Invoke(EventType::OnDemoBoundsDetermined);
                else
                    LOG_WARN("T6: the film's length isn't known yet, so the timeline has no view to click in");
                const auto demo_pause = Functions::FindDvar("demo_pause");
                lastGamePause = demo_pause && demo_pause->current.enabled;
                if (Components::Playback::IsPaused() != lastGamePause)
                    Components::Playback::TogglePaused();
            }
        }
        Props::Update();
        UpdateHud(playing);

        // (the file found after the wait: the length is told then)
        if (playing && !boundsTold && GetFilmTimes())
        {
            boundsTold = true;
            LOG_INFO("T6: the film's length is known now");
            Events::Invoke(EventType::OnDemoBoundsDetermined);
        }
        if (!playing)
        {
            UpdateRecording(false, false);
            pendingSeek = -1;
            if (seekState != SeekState::Idle)
                FinishSeek("the film stopped");
            return;
        }

        if (UpdateSeek())
        {
            UpdateRecording(true, true);
            return;
        }
        // (the pause is the capture's while it records)
        if (UpdateRecording(true, IsSeeking()))
            return;

        const auto demo_pause = Functions::FindDvar("demo_pause");
        if (!demo_pause || demo_pause->type != Structures::DVAR_TYPE_BOOL)
            return;

        const bool gamePause = demo_pause->current.enabled;
        const bool editorPause = Components::Playback::IsPaused();
        if (gamePause != lastGamePause)
        {
            // the theater changed it (its own pause, or the film's end): the editor follows
            lastGamePause = gamePause;
            if (editorPause != gamePause)
                Components::Playback::TogglePaused();
        }
        else if (editorPause != gamePause)
        {
            // the editor changed it: the theater follows
            demo_pause->current.enabled = editorPause;
            demo_pause->modified = true;
            lastGamePause = editorPause;
        }
    }
}  // namespace IWXMVM::T6::Theater
