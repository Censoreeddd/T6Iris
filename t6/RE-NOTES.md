# T6 reverse-engineering notes

`t6mpv43.exe` as the Redacted LAN client runs it: PE timestamp 0x536E8FB4, image base 0x400000,
SHA-256 `770318175f0161aa7a1ff0f9a5530336836a99e72900d7608a63973e56004adf`. Not packed (`.text` entropy 6.7; only the
small `.conceal` section is encrypted). Renders with Direct3D 11. Window class `CoDBlackOps`.

Addresses are virtual addresses in that image. **Confirmed** means T6iris uses it and it was seen working in the game;
**static** means read from the disassembly only.

## Redacted plugin loader (confirmed)

`steam_api.dll` loads `Plugins\*.Red32n` (32-bit DLLs) and looks up nine exports: `Uninit`, `PreInit`, `PostInit`,
`AuthorInfo`, `ExtendedInfo`, `OfficialMod`, `DependencyCount`, `GetDependency`, `SendMessageB`. Without
`SendMessageB` it stops before calling any. It calls `DependencyCount`, `AuthorInfo`, `PreInit` (~0.5 s in, before
any window or D3D device) and `PostInit` on the main thread, `Uninit` at exit.

## Input (confirmed)

| Address | What |
| --- | --- |
| 0x444E50 | `IN_Init`: registers `in_mouse` (dvar pointer at 0x2B69940), sets mouseInitialized (byte 0x2B69969) |
| 0x433CB0 | `IN_Frame` (signature in Signatures.hpp). T6iris return-patches it while the editor has the mouse |
| 0x8D8610 | `IN_MouseMove`: GetCursorPos, delta from 0x2B69960/64, `CL_MouseEvent` (0x416EB0), recentres (0x5EAF80) |
| 0x5F5AE0 | `IN_ActivateMouse`; mouseActive is byte 0x2B69968 |
| 0x5CE710 | `IN_DeactivateMouse` |
| 0x507680 | `IN_MouseEvent` (buttons; old state 0x2B6995C) |
| 0x46C650 | `MainWndProc` (core hooks the window procedure it reads from the window; same address) |
| 0x2B6ED88 | `g_wv.hWnd` |

No raw input: the mouse is read with GetCursorPos/SetCursorPos.

## Console, dvars, keys (confirmed)

| Address | What |
| --- | --- |
| 0x5C6F10 | `Cbuf_AddText(localClientNum, text)`; T5's signature matches. `0x699710()` gives the client number |
| 0x8C8950 | `Dvar_FindVar`, name in eax: djb2 (5381, `h*33 + tolower(c)`), then `Dvar_FindMalleableVar` |
| 0x8C88E0 | `Dvar_FindMalleableVar`, hash in esi; table at 0x2A1EC48 (1024 buckets), lock 0x2A1AC34 |
| 0x8C97C0 | `Dvar_RegisterVariant`; 0x8C9680 re-register, 0x8C8F90 register new |
| 0x4E0170 | `Dvar_RegisterBool` (type 1); 0x4445C0 float (2); 0x473320 vec3 (4); 0x5F24E0 int (6); 0x59ACB0 enum (7) |
| 0x6226F0 | `Dvar_GetBool` |
| 0x11C7844 | `keyCatchers` (Key_AddCatcher 0x4C5D00, Key_SetCatcher 0x48DC40). 1 console, 0x10 UI, 0x20 chat. 0 at the main menu |
| 0x11C7848 | connection state (the theater commands need >= 4) |
| 0x5B3070 | `Cmd_AddCommand(name, handler, cmd_function_t*)` |
| 0x5EFBA0 | `Sys_GetValue(4)` gives the command arguments: argc at +0x84, argv at +0xA4 (as T5) |

`dvar_t`: name 0x00, hash 0x08, flags 0x0C, type 0x10, modified 0x14, current 0x18, latched 0x28, reset 0x38, hashNext
0x58. Types: 0 invalid, 1 bool, 2 float, 3 vec2, 4 vec3, 5 vec4, 6 int, 7 enum, 8 string, 9 color (1, 2, 4, 6, 7 seen).
Defaults read in the game: cg_fov 65, timescale 1, com_maxfps 0.

## Theater (confirmed)

Commands (registered at 0x498D70): `demo_play` 0x927D60, `demo_stop` 0x928040, `demo_forward` 0x928080,
`demo_back` 0x928110, `demo_jumptostart` 0x9281A0, `demo_switchplayer` 0x927560, dolly marker commands
0x928E50-0x9295C0, clips and segments 0x928350-0x928630. `demo_pause` is a bool dvar (pointer at 0x30D0904);
`demo_enabled`, `demo_maxTimeScale`, `demo_enableDollyCam` and others are dvars too.

| Address | What |
| --- | --- |
| 0x30D3404 | demo state: 0 none, 1 loading, 2 playing (confirmed by the probe; every theater command requires 2) |
| 0x3183A00 | pointer to the theater's playback structure, null until the film has loaded (~5 s after state 2); 0x3183A04 a flag |
| +0x60580 | snapshot table, 0x30 bytes each: +0x00 offset in the unpacked stream, +0x04 offset in the .demo file (where a chunk starts), +0x08 film time (ms), +0x10 a count, +0x14 size, +0x18 saved state |
| +0x63490 | snapshots recorded so far: grows as the film plays (one every 10 s of film), so only played parts can be jumped to |
| +0x634A0 | snapshot last jumped to (-1 before any) |
| 0x675D10 | `demo_forward` -> target snapshot (0x6C88C0) -> 0x92FB30 |
| 0x51C0B0 | `demo_back` -> target snapshot (0x5A8F90) -> 0x92FB30 |
| 0x92FB30 | jump to snapshot(index): restores its state and file position (writes +0x634A0) |
| 0x56CDF0 | called by `demo_jumptostart` with a float |
| +0xD4C00 | **the theater's camera mode**: 0 first person, 1 third person, 2 free (the change-camera key 0x48E770 cycles 0, 1, 2). Read by the menus' `demoisanyfreemovecamera` (0x6D7B20) |
| +0xD4C04 | the free camera's kind: 2 the dolly camera (`demoisdollycamera`, 0x63D6A0), 1 tested by 0x560040; written at 0x41C4FA |
| 0x430BC0 | the theater's free camera's view (localClientNum): run by the frame (0x51DCD0) after `CG_CalcViewValues` and the theater's camera (0x6DB530) when `demoisanyfreemovecamera`; writes the refdef's origin and axis. 0x6D8EF0 is another view run there (when cg+0x6496C & 1). T6iris writes its camera after both |
| +0x63569 / +0x6356A | the theater's HUD and its controls hidden (bytes, 1 hidden): F2 and F4 toggle them in its key handler 0x5F3E30, whose jump table (0x5F4520, by key - 1 at 0x5F454C) has F2 0xA8, F3 0xA9 (change camera, 0x48E770), F4 0xAA, Space 0x20; its screenshot (0x92B5BA) sets both to 1 for a moment |
| 0x405DE0 | `Demo_SetCameraMode(localClientNum, mode)`: going into the free camera starts it (from the third person where that view was, saved at +0xEE734); leaving it ends it (0x6FB410). T6iris calls it while the editor flies the camera (confirmed by static reading only) |
| 0x927D60 | `demo_play <name>`: the game itself sends `demo_play <ui_demoname>` (dvar pointer 0x294E20C) only from the theater lobby, after setting up the session's players (0x675E90, 0x7F4840). Sent from the main menu it crashed the game. The film it opens is `<name>.demo`; the lobby's own film is `Film` (ui_demoname's default), which `downloaddemofile` (0x66DA80, `<pooled> <fileid> <size> <name> <category>`) writes from the file share and sets ui_demoname to |
| 0x92AF00 | `xpartyplaydemo`: with Com_SessionMode_IsMode(5) (0x6F7570, the mask at 0x2560890) or the party session (0x12A7000) running, 0x675E90 checks the session (+0x9590 == 1, +0x958C, +0x9584 set), sets up its 18 players (+0x208, 0x148 each) and sends `demo_play <ui_demoname>`. What the Demo tab sends from the Theater's lobby, after `set ui_demoname <file>` |
| 0x3205678 | The film in memory: pointer, size (+4), read position (+8). Set up by 0x6A45B0(size) (VirtualAlloc, 0x5F6970), filled by `downloaddemofile`'s callback (0x92AB70), read by 0x50A980 (memcpy from pointer + position). The theater plays from it; with it null demo_play crashed there (0xA72D44, from 0x7007F0 → 0x680AF0) |
| 0x8A3CD0 | `setactivemenu <theater\|private>`: the lobby menus' switch (0x49BAA0(controller, type)) with type 21 (TheaterLobby, which sets up the party as `TheaterLobby`, mode 5, 0x89D320) or 18. What the Demo tab sends from the main menu. It leaves the party session running (+0x9590 1, the theater's mode) but not hosted (+0x958C, +0x9584 0) until `xstartpartyhost` (0x4E9880 → 0x6BA0B0); seen working: the film plays about 8 s after PLAY |
| 0x977330 | LUI `SetTheaterFileInfo`: the lobby's chosen film (name, description, category, duration, isPooled, matchID, fileid) copied to 0x3208198 by 0x585130 |
| 0x11C7D6C | number of local clients (1 in a film) |
| 0x11C7D74 | pointer to cg (one local client) |
| cg+0x58 | **film time, ms**: +1000 a second at timescale 1, frozen while paused, jumps with the snapshot on a skip (confirmed) |
| cg+0x5C | the previous snapshot's time (~60 ms behind +0x58); +0x60, +0x64 snapshot numbers (time / 100) |

Seen in the probe (Nuketown film): playback starts ~0.7 s, snapshots at 658, 10601, 20601, 30601, 40602 ms; at the
film's end (50000) the theater sets `demo_pause` itself.

Seeking plan: to a time already played, jump to the snapshot before it (0x92FB30), then run forward to the exact time;
to a time not played yet, run forward fast (timescale) until it's reached, as the theater records snapshots on the way.

## Demo files

Redacted keeps films in `Plugins\OpenNetStorage\Cloud\FILM_MP\<name>.demo` with `.summary`, `.tags` and `.thumbnail`
beside them. A `.demo` is a series of chunks, each `u32 compressedSize, u32 uncompressedSize` and then raw deflate
(zlib with wbits -15); the last chunk has the match's metadata (game type, map), and 8 bytes end the file. The test
film (Nuketown, 70,818 bytes) has 17 chunks, 85,468 bytes unpacked. `.summary` is one raw deflate stream (~100 KB).

The metadata chunk starts `u32 13, u32 startTime, u32 endTime` (400 and 50050 ms in the test film, which the probe saw
end at 50000), then the game type, map name, display name ("NUKETOWN 2025") and game type name, then a list of
segments (start times 6600, 7100, ... each with the end 50050). The 8-byte footer is `u32 261, u32 13`. A chunk's first
u32 is not a time (it rises unevenly: 919, 10983, 21479, ...).

## View (confirmed)

| Address | What |
| --- | --- |
| 0x113F18C | pointer to cg, the client game structure with the view (1328 references). Not 0x11C7D74, which has the film's time |
| cg+0x4D8A0 | refdef: tan(fov/2) x,y at +0x10/+0x14, fov (degrees, cg_fov's 65) +0x20, origin +0x24, axis +0x34 |
| cg+0x648F0 | the view's angles, from which the axis is made |
| 0x7D7120 | `CG_CalcViewValues(localClientNum)`: writes the origin and calls AnglesToAxis (0x571A10) for the axis |
| 0x42A510 | registers the camera dvars (cg_fov pointer 0x10AA23C, cg_thirdPerson 0x10A802C) |

Found from a scan of memory for rotations while a film played: the axis at cg(0x11C7D74)+0x21AEF4 turned with the view,
with tan(fov/2) 0.8494, 0.4778 (16:9) and the fov 65 beside it; the pointer at 0x113F18C is 0x4D8A0 before that refdef.

## Bones (static; T6iris's bone camera)

| Address | What |
| --- | --- |
| 0x1140878 | pointers to each local client's cg_entities, allocated at 0x46B630 ("cg_entitiesArray", 0x188000 bytes, 1,792 of 0x380) |
| centity +0x2C | its place; +0x1DC its number (the DObj handle); +0x1E0 flags (4 crouched, 8 prone, as AimTarget_GetTagPos reads them); +0x2B4 its type (word, 1 a player) |
| 0x4DA190 | `Com_GetClientDObj(handle, localClientNum)`: 0x701 handles a client, table at 0x262D1C0, DObjs of 0x7C at 0x25EF1B8 |
| 0x4FC740 | `CG_DObjGetWorldTagMatrix(pose, obj, tagName, axis[3][3], origin)`; 0x4A1210 the same for the origin only (`CG_DObjGetWorldTagPos`) |
| 0x46B2E0 | `DObjGetBoneIndex`, as T5's |
| 0x6B8EB0 | `SL_FindString(text)`: the script string's number, 0 if there's none; 0x5EE820 `SL_GetString(text)` makes one |
| 0x794DA0 | `AimTarget_GetTagPos`: how the game itself reads an entity's tag (players through 0x444350) |
| 0x5B0F50 | a player's name (localClientNum, clientNum, out, size, bool withClan), from the snapshot's client states (cg [0x11C7D74]: +0x18 their array, 0x120 each, +0x1C its length, +0x2A7C the count, +0x2A8C the first); 1 if found. LUI's `GetFullPlayerName` (0x980B60) calls it |

## Lights (static; T6iris's scene lights)

| Address | What |
| --- | --- |
| 0x726F00 | `R_AddOmniLightToScene(origin, radius, r, g, b, intensity)`: sRGB colour, made linear and times intensity * 32 into the scene's list (type 5) |
| 0x727110 | `R_AddSpotLightToScene(origin, axis[3][3], radius, innerFraction, startRadius, endRadius, r, g, b, intensity, def)` (type 2) |
| 0x3553BFC | the scene's added lights this frame (32 at most), 0x160 bytes each from 0x3553C00; `r_dlightLimit` (0x343488C, at most 4) of them light a surface (0x73E5E0) |
| r_lightTweakSunColor | type 11, a linear colour of three floats (registered at 0x5C5050); the sun's dvars apply when marked modified (0x712460, 0x74E910) |

## Frame timing, debug views (found for passes, which were taken out again) and the party

- The game has a render thread of its own: the thread names at 0xD5EA60 are Main, Backend, Worker0 to 7. Present runs
  on Backend, a frame or so behind Main, so a dvar changed at a Present shows a frame or two later. Passes would have to be
  recorded one after another for that reason.
- `fixedtime` (dvar pointer 0x2562B40) is read in Com_Frame at 0x88D017: when set, the frame's msec is fixedtime, at
  least 1 (it can't hold time still).
- Debug views: `r_debugShader` (enum at 0x1064B74: none, normal, basisTangent, basisBinormal, basisNormal, lmapDir),
  `r_zbufferDebug` (bool) with `r_zbufferDebugRange` (vec2). Draw switches: `r_add_bsp`, `r_add_staticmodel`,
  `r_sceneents_model`, `fx_draw`, `cg_drawGun`, `r_clearColor`. There is no `r_clear`.
- Resolution dvars for later (render size): `r_mode`, `r_fullscreen`, `r_resolution`, `r_aspectRatioWindow`, and the
  `vid_restart` command.
- A party with fewer players than `party_minplayers` searches for more every `party_searchInterval` (0x6AA860, then
  0x4C2440 and 0x6FD1D0), which calls the LUI's global `SetupMatchmakingQuery` (0x4D66F0). The Theater's menus have
  none; reporting that, the game read a null string and crashed at 0x6E6A61, 12 to 16 s after a film was started from
  the main menu. The Demo tab sets `party_minplayers` to 1, as the game's own Theater lobby has it.
- With `party_minplayers` 1 the party of that lobby had enough players to start a match: `xpartygo` (0x5CD780) came
  and loaded the party's map (0x66AEE0, 0x7F5DC0), the client game began again under the film and the game crashed at
  0x626C88 (cg + 0x699D4 not set yet). The Demo tab now also sets `party_dedicatedMergeMinPlayers` to 32 and lets no
  `xpartygo` through in the lobby it opened.

## Film grading

- 0x72B7B0: with `r_filmUseTweaks` (pointer 0x34346B8) the frame's grade is `r_filmTweakEnable` (+0x24) and 14 vec4
  dvars (+0x28 on, 0x10 each): `vc_RS`, `vc_RE` (range start and end; defaults (0, 0.5, 0, 0.75) and
  (0.5, 1, 0.25, 1)), `vc_SMR/G/B`, `vc_MMR/G/B`, `vc_HMR/G/B` (shadow, midtone and highlight matrices, identity rows by
  default), `vc_FGM` (1,1,1,1), `vc_FSM` (0,0,0,1), `vc_FBM` (0); otherwise it copies the map's vision (0x44 dwords).
  The vec4s are registered by 0x643E80 (type 5, -100 to 100). The shader constants are colorMatrixR/G/B, colorBias,
  colorTintBase and colorTintDelta, as in the older games.
- The match still started without `xpartygo`: the party's auto start (0x7F8EE0, the session in eax, from 0x4C52A0)
  counts a lobby with `party_minplayers` down (AutoStart_LOBBY_FLAG_WAITING, session + 0x9620 4, then 0x10) and
  loads the party's map itself (0x66AEE0). T6iris returns from it at once in the lobby the Demo tab opened.

## The cursor

BO2 puts the cursor's count back to 0 or -1 with ShowCursor in a loop (0x8D86F0, 0x4D1750), as many calls as the
count is off by. Core's ShowCursor(TRUE) on every window message (for the D3D9 games) raised it by thousands, so each
of those loops ran that long: the mouse lagged and flickered over the editor. For a D3D11 game core only shows the
cursor again when it's hidden.

## Snapshots

- The snapshots are a ring of 250 at theater + 0x60580 (0x30 each), the next written at +0x63490 (the count modulo
  250, -1 before the first); 0x92C680 writes them and marks with the time -1 those whose part of the film's stream (a
  0x500000 ring) was written over.
- They're written only while 0x92C5D0 says one is due, which needs 0x591C10: a match may be recorded, i.e. not
  (0x2366AA4's int >= 2), the session's modes (0x2562AC4) with the online game (bit 2, the LUI's
  SessionModeSetOnlineGame; 0 offline, 1 system link, 3 private), and `demo_enabled` (with `demo_recordPrivateMatch`
  too when the party's mode has bit 1). Films started from the Demo tab's lobby (opened from the main menu) had no
  snapshots at all: 0 of them, the next -1. T6iris sets bit 2 and `demo_enabled` for them.

