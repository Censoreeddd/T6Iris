#include "StdInclude.hpp"
#include "TheaterProbe.hpp"

#include "Functions.hpp"

namespace IWXMVM::T6::TheaterProbe
{
    namespace
    {
        constexpr std::uint32_t T6MPV43_TIMESTAMP = 0x536E8FB4;

        constexpr std::uintptr_t DEMO_STATE = 0x30D3404;   // 2 while a film plays (every theater command checks it)
        constexpr std::uintptr_t DEMO_STATE_PREV = 0x30D3400;
        constexpr std::uintptr_t THEATER = 0x3183A00;      // pointer to the playback structure
        constexpr std::uintptr_t THEATER_FLAG = 0x3183A04; // byte
        constexpr std::uintptr_t CG_COUNT = 0x11C7D6C;
        constexpr std::uintptr_t CG_ARRAY = 0x11C7D74;     // the seek code takes cg from here

        constexpr std::uintptr_t KEYFRAMES = 0x60580;      // 0x30 bytes each
        constexpr std::uintptr_t KEYFRAME_SIZE = 0x30;

        bool IsKnownBuild()
        {
            static const bool known = [] {
                const auto base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleA(nullptr));
                const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
                const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
                return nt->FileHeader.TimeDateStamp == T6MPV43_TIMESTAMP;
            }();
            return known;
        }

        // a 32-bit value at an address that may not be readable
        bool Read(std::uintptr_t address, std::uint32_t& value)
        {
            if (address < 0x10000)
                return false;
#if defined(_MSC_VER)
            __try
            {
                value = *reinterpret_cast<const volatile std::uint32_t*>(address);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
#else
            value = *reinterpret_cast<const volatile std::uint32_t*>(address);
            return true;
#endif
        }

        std::string Field(const char* name, std::uintptr_t address)
        {
            std::uint32_t value = 0;
            if (!Read(address, value))
                return std::format("{}=?", name);
            return std::format("{}={}", name, static_cast<std::int32_t>(value));
        }

        // Finding the theater's camera mode: small whole numbers (0-16, as a mode is) in cg, the theater's structure
        // and the theater's globals, compared four times a second; each change is logged while that field has changed
        // at most a few times, so a value that ticks along is muted and a switch of mode stands out when it's made
        struct Watched
        {
            const char* name;
            std::vector<std::uint32_t> previous;
            std::vector<std::uint8_t> changes;
            std::uintptr_t base = 0;
        };
        std::array<Watched, 4> watched{Watched{"cg"}, Watched{"theater"}, Watched{"demo globals"}, Watched{"theater globals"}};
        std::uint64_t lastWatch = 0;
        constexpr std::uint8_t MAX_LOGGED_CHANGES = 4;

        // whether all of [base, base + size) is committed, readable memory
        bool IsReadable(std::uintptr_t base, std::size_t size)
        {
            auto address = base;
            while (address < base + size)
            {
                MEMORY_BASIC_INFORMATION info{};
                if (!VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) || info.State != MEM_COMMIT ||
                    (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
                    !(info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                      PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
                    return false;
                address = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
            }
            return true;
        }

        void Watch(Watched& w, std::uintptr_t base, std::size_t size)
        {
            if (!base || !IsReadable(base, size))
                return;
            if (w.base != base || w.previous.size() != size / 4)
            {
                // a new structure: start over
                w.base = base;
                w.previous.assign(reinterpret_cast<const std::uint32_t*>(base),
                                  reinterpret_cast<const std::uint32_t*>(base) + size / 4);
                w.changes.assign(size / 4, 0);
                LOG_INFO("T6 watch: watching {} at {:#x} ({:#x} bytes)", w.name, base, size);
                return;
            }
            const auto now = reinterpret_cast<const std::uint32_t*>(base);
            for (std::size_t i = 0; i < w.previous.size(); ++i)
            {
                const auto before = w.previous[i];
                const auto after = now[i];
                if (before == after)
                    continue;
                w.previous[i] = after;
                if (before > 16 || after > 16 || w.changes[i] == 0xFF)
                    continue;
                if (++w.changes[i] <= MAX_LOGGED_CHANGES)
                    LOG_INFO("T6 watch: {}+{:#x}: {} -> {}{}", w.name, i * 4, before, after,
                             w.changes[i] == MAX_LOGGED_CHANGES ? " (muted from now on)" : "");
            }
        }

        void WatchForCameraMode()
        {
            const auto now = GetTickCount64();
            if (now - lastWatch < 250)
                return;
            lastWatch = now;

            std::uint32_t cg = 0, theater = 0;
            Read(0x113F18C, cg);
            Read(THEATER, theater);
            // cg up to past the fields the view code uses (0x699BC), the theater's structure, and the globals
            // around the demo state (0x30D3404) and the theater pointer (0x3183A00)
            Watch(watched[0], cg, 0x70000);
            Watch(watched[1], theater, 0x64000);
            Watch(watched[2], 0x30D0000, 0x10000);
            Watch(watched[3], 0x3180000, 0x10000);
        }

        std::uint32_t lastState = 0xFFFFFFFF;
        std::uint64_t lastLog = 0;
        std::uint64_t lastScan = 0;

        // Finding the refdef: cg holds the view's axis, a 3x3 rotation (rows of length 1 at right angles, determinant
        // +1). Every one in cg's memory is logged with what's around it: in T5's refdef the view's origin is 0x14
        // bytes before the axis and tan(fov/2) for x and y 0x24 and 0x20 before it. Which one moves with the view says
        // which is the refdef.
        bool IsRotation(const float* m)
        {
            for (int i = 0; i < 9; ++i)
                if (!(m[i] >= -1.001f && m[i] <= 1.001f))
                    return false;
            const auto dot = [&](int a, int b) { return m[a * 3] * m[b * 3] + m[a * 3 + 1] * m[b * 3 + 1] + m[a * 3 + 2] * m[b * 3 + 2]; };
            for (int r = 0; r < 3; ++r)
                if (std::abs(dot(r, r) - 1.0f) > 0.002f)
                    return false;
            if (std::abs(dot(0, 1)) > 0.002f || std::abs(dot(0, 2)) > 0.002f || std::abs(dot(1, 2)) > 0.002f)
                return false;
            const float det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
                              m[2] * (m[3] * m[7] - m[4] * m[6]);
            // not the identity, which unused matrices are
            const bool identity = std::abs(m[0] - 1) < 1e-4f && std::abs(m[4] - 1) < 1e-4f && std::abs(m[8] - 1) < 1e-4f;
            return det > 0.99f && det < 1.01f && !identity;
        }

        // Once: who points at the refdef found at cg+0x21AEC0 (the axis at +0x21AEF4 turned with the view, with the
        // origin and tan(fov/2) beside it as in T5's refdef). cg_t is 0x8A3E0 bytes, so it isn't in cg; no code
        // addresses it from cg either. So: its absolute address, its allocation, and every pointer in the game's
        // .data into the 4 MB before it, which names the structure it's in.
        constexpr std::uintptr_t FOUND_REFDEF = 0x21AEC0;
        bool ownersLogged = false;

        void LogRefdefOwners(std::uintptr_t cg)
        {
            const auto refdef = cg + FOUND_REFDEF;
            MEMORY_BASIC_INFORMATION info{};
            VirtualQuery(reinterpret_cast<void*>(refdef), &info, sizeof(info));
            LOG_INFO("T6 owners: cg={:#x} refdef={:#x} allocation base={:#x} region base={:#x} size={:#x}", cg, refdef,
                     reinterpret_cast<std::uintptr_t>(info.AllocationBase),
                     reinterpret_cast<std::uintptr_t>(info.BaseAddress), info.RegionSize);

            // the game's .data, from its section header
            const auto base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleA(nullptr));
            const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
            auto section = IMAGE_FIRST_SECTION(nt);
            for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
            {
                if (std::memcmp(section->Name, ".data", 6) != 0)
                    continue;
                const auto start = reinterpret_cast<std::uintptr_t>(base) + section->VirtualAddress;
                const auto end = start + section->Misc.VirtualSize;
                int found = 0;
                for (std::uintptr_t at = start; at + 4 <= end && found < 40; at += 4)
                {
                    const auto value = *reinterpret_cast<const std::uintptr_t*>(at);
                    if (value <= refdef && refdef - value < 0x400000)
                    {
                        ++found;
                        LOG_INFO("T6 owners: [{:#x}] = {:#x}, refdef at +{:#x}", at, value, refdef - value);
                    }
                }
                LOG_INFO("T6 owners: {} pointers in .data ({:#x} to {:#x})", found, start, end);
            }
        }

        void ScanCg(std::uintptr_t cg)
        {
            if (!ownersLogged)
            {
                ownersLogged = true;
                LogRefdefOwners(cg);
            }
            // the committed memory from cg on, up to 4 MB
            std::uintptr_t end = cg;
            MEMORY_BASIC_INFORMATION info{};
            while (end - cg < 0x400000 && VirtualQuery(reinterpret_cast<void*>(end), &info, sizeof(info)) &&
                   info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
            end = std::min(end, cg + 0x400000);

            int found = 0;
            for (std::uintptr_t at = cg + 0x40; at + 36 <= end && found < 24; at += 4)
            {
                const auto m = reinterpret_cast<const float*>(at);
                if (!IsRotation(m))
                    continue;
                ++found;
                const auto f = [&](std::intptr_t offset) { return *reinterpret_cast<const float*>(at + offset); };
                LOG_INFO("T6 scan: axis at cg+{:#x} fwd=({:.3f},{:.3f},{:.3f}) up=({:.3f},{:.3f},{:.3f}) | -0x14=({:.1f},{:.1f},{:.1f}) "
                         "-0x24/-0x20=({:.4f},{:.4f}) -0x1c={:.3f}",
                         at - cg, m[0], m[1], m[2], m[6], m[7], m[8], f(-0x14), f(-0x10), f(-0xC), f(-0x24), f(-0x20),
                         f(-0x1C));
                at += 32;  // past this one
            }
            LOG_INFO("T6 scan: {} axes in cg's first {:#x} bytes", found, end - cg);
        }
    }  // namespace

    void OnFrame()
    {
        if (!IsKnownBuild())
            return;

        std::uint32_t state = 0;
        if (!Read(DEMO_STATE, state))
            return;
        // found: the mode is at theater + 0xD4C00 (Structures.hpp), so the watch is off
        constexpr bool WATCH_FOR_CAMERA_MODE = false;
        if (WATCH_FOR_CAMERA_MODE && state == 2)
            WatchForCameraMode();

        const auto now = GetTickCount64();
        const bool changed = state != lastState;
        if (!changed && (state != 2 || now - lastLog < 1000))
            return;
        lastState = state;
        lastLog = now;

        std::uint32_t theater = 0, cg = 0, flag = 0;
        Read(THEATER, theater);
        Read(CG_ARRAY, cg);
        Read(THEATER_FLAG, flag);

        std::string line = std::format("T6 probe: demoState={}{} {} theater={:#x} flag={} {}", state,
                                       changed ? " (changed)" : "", Field("prev", DEMO_STATE_PREV), theater,
                                       flag & 0xFF, Field("cgCount", CG_COUNT));
        if (theater)
        {
            line += " | " + Field("t+63490", theater + 0x63490) + " " + Field("t+634a0", theater + 0x634A0) + " " +
                    Field("t+6057c", theater + 0x6057C) + " " + Field("t+101710", theater + 0x101710) + " " +
                    Field("t+00", theater + 0x0) + " " + Field("t+04", theater + 0x4);

            std::uint32_t count = 0;
            Read(theater + 0x63490, count);
            for (std::uint32_t i : {0u, 1u, 2u, count > 0 && count < 4096 ? count - 1 : 0u, count < 4096 ? count : 0u})
            {
                const auto entry = theater + KEYFRAMES + i * KEYFRAME_SIZE;
                line += std::format(" | kf[{}]:", i);
                for (std::uintptr_t offset = 0; offset < 0x18; offset += 4)
                    line += " " + Field(std::format("+{:x}", offset).c_str(), entry + offset);
            }
        }
        // the scan found the refdef (cg [0x113F18C] + 0x4D8A0, Structures.hpp); off, as it stalls a frame
        constexpr bool SCAN_FOR_REFDEF = false;
        if (SCAN_FOR_REFDEF && cg && state == 2 && now - lastScan >= 3000)
        {
            lastScan = now;
            ScanCg(cg);
        }
        if (cg)
        {
            line += " | cg:";
            for (std::uintptr_t offset : {0x0u, 0x4u, 0x8u, 0x48u, 0x4Cu, 0x50u, 0x54u, 0x58u, 0x5Cu, 0x60u, 0x64u})
                line += " " + Field(std::format("+{:x}", offset).c_str(), cg + offset);
        }

        {
            std::uint32_t viewCg = 0, viewTime = 0;
            Read(0x113F18C, viewCg);
            if (viewCg)
                Read(viewCg + 0x4808C, viewTime);
            line += std::format(" | cg.time={}", static_cast<std::int32_t>(viewTime));
        }

        const auto demo_pause = Functions::FindDvar("demo_pause");
        const auto timescale = Functions::FindDvar("timescale");
        line += std::format(" | demo_pause={} timescale={}", demo_pause ? demo_pause->current.enabled : false,
                            timescale ? timescale->current.value : -1.0f);

        LOG_INFO("{}", line);
    }
}  // namespace IWXMVM::T6::TheaterProbe
