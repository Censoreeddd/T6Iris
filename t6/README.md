# T6iris (Black Ops II multiplayer, Redacted)

The Black Ops II binding of Iris Studio, built as a plugin for the Redacted LAN client: `T6iris.Red32n`, which
Redacted loads from the game's `Plugins` folder. It runs in `t6mpv43.exe` (PE timestamp 0x536E8FB4); the addresses it
uses are found by signature and listed in [RE-NOTES.md](RE-NOTES.md).

## What works

Press PLAY on a film in the editor's Demo tab (from the main menu or the Theater's lobby), or start one from the game's Theater; F1 shows or hides the editor.

| Area | In T6iris |
| --- | --- |
| Editor | The UI over the game (D3D11), the game view (scaled with a Catmull-Rom filter), the mouse and keyboard routed between the editor and the game |
| Demo tab | From the Theater's lobby, PLAY puts the film's file in the theater's film buffer (where the lobby's download leaves a film; the theater plays from memory) and starts it the way the lobby's Start Film does (`ui_demoname`, then `xpartyplaydemo`). From the main menu it opens the Theater's lobby first (`setactivemenu theater`, then `xstartpartyhost`) and starts the film once the lobby is up, about 8 seconds in all; while a film plays it says to leave it first |
| Films | Found at once by the film's bytes in the theater's film buffer (the lobby's download), else by matching the theater's snapshots to the files in `Plugins\OpenNetStorage\Cloud\FILM_MP`; their name, length and keyframes kept by file |
| Playback | Pause, speed, and seeking anywhere on the timeline: back through the theater's own snapshots (`demo_back`, `demo_jumptostart`), then on at a raised timescale (up to 64 times) to the exact time; forward, to a snapshot already recorded on the way when the film played there before; the editor's game view keeping its frame meanwhile |
| Cameras | Free, orbit and the dolly (the campath) write the view after the game's own view, the theater's camera and its free camera. While one flies, the theater's camera is put in its free mode (no gun or first-person HUD) and back afterwards. The third person camera is the theater's own, following its player from behind. The bone camera follows a player's bone (the game's tag lookup), its players listed by name |
| Campath on screen | The line, the nodes (clicked to select them), the selected node's gizmo (handles to move it along its own axes or the world's, rings to turn it; the view's top-left buttons or a double click switch) and the orbit camera's axes, drawn by ImGui over the game view; no depth (they show through walls) |
| Recording | Video or image sequences through ffmpeg: the frame read back before the UI is drawn over it, each frame the capture's frame time of the film (`fixedtime`) |
| HUD | While the editor is shown or records, the theater's controls bar is hidden and its HUD (the killfeed, score and the rest) follows the Look tab's 2D Elements, off by default; with the editor hidden its own F2 and F4 work as in the game. Crosshair, shellshock, player HUD and killfeed switches from the Look tab |
| Depth of field | The Look tab's Depth of Field through the `r_dof_` dvars: with `r_dof_tweak` on the renderer takes the frame's DoF from them (0x729D90) whatever `r_dof_enable` says; off, the game's own (aiming down sights). The defaults are the dvars' reset values. Its Type also offers the mod's own Directional and Radial blur (Features_ScreenBlur): a shader over the whole frame drawn into the back buffer before it's captured, the editor drawn or ReShade's effects run (`D3D11::SetScreenBlur`), only while a demo plays. The Lens (Bokeh) type (Features_LensBlur) blurs by depth as a camera lens would: the circle of confusion from the focus distance, focal length (mm, full frame) and f-number, gathered in the aperture's shape with bright spots weighted up, from `$floatz`, the game's linear depth (the third render target image, 0x3A24A88 + 2 * 0x50; its shader resource view at +0), which the game fills only for its own effects that need it (it read 0 everywhere in a film), else from `$scene_depth` (image 11), the depth buffer, as `r_znear / (1 - z)` (or `/ z` when reversed, told by where most of the picture is). A probe shader reads the depth at the pin, the centre and a grid back for the editor and the log. |
| Motion blur | The Record tab's Motion Blur (Features_Shutter): a shutter speed and samples. A capture renders that many frames for each it records, the first moving on to the shutter's opening and the others stepping through it in whole milliseconds (CaptureManager::GetShutterPlan, OnGameFrame); each is blended in linear light into an accumulation on the render thread (D3D11::DrawShutter) and only the last is recorded. A shutter longer than a frame blends the frames before it from a ring of up to 16. While the film plays in the editor, an estimate: each frame blended in by the part of the shutter's time it moved on. |
| Camera shake | The Camera tab's Camera Shake (Features_CameraShake, Components::CameraShake): smooth noise by the film's time (three octaves, a step's bob for Walking and Running) added to the view's angles and to its position along its own axes, in Camera.cpp as the view is written, for the editor's cameras and over the game's view (once a frame, as the theater's camera may write the view again). Intensity, Rotation and Position are keyframeable. |
| Lens flare | The Camera tab's Lens Flare (Features_LensFlare, Components::LensFlare): a glow, rays, an anamorphic streak, ghosts along the line through the picture's centre and a rainbow halo, worked out per pixel from where the light is and added to the frame (D3D11::DrawLensFlare). The light is the sun (Camera.cpp projects r_lightTweakSunDirection into the view each write, faded where the depth buffer, $scene_depth, isn't the sky around it) a spot of the map picked on the picture, or the brightest lights found on each frame. Intensity is keyframeable. |
| Tracking | The lens's Track focus and the lens flare's Follow source (Types::ScreenTrack, Components::Tracking): a player's or an actor's bone (GetTargetBone) or a point of the map, projected each frame through the view Camera.cpp reports as it writes it; the focus distance is its depth along the view, no keys needed. A click on the picture picks the nearest player (by the bone, the chest or the middle) or else the spot of the map there, its depth read from the depth buffer by the probe shader (D3D11::RequestDepthAt) and taken back into the world. The lens flare's Bright lights (auto) source finds lights on each frame on the GPU: the frame in a 64x36 grid of cells (each its brightest of 16 taps over a threshold), then the brightest cells apart from each other into an 8x1 target the flare shader reads, a flare on each. |
| Sky replacement | The Look tab's Sky (Features_SkyReplace, Components::SkyReplace): drawn first over the frame (before the flares and the depth of field) wherever the depth buffer ($scene_depth) has its far value or 0 (nothing drawn: the sky), softened over a few taps; each pixel's direction from the view Camera.cpp reports, into a 360 degree panorama (equirectangular), a photo standing on the horizon mirrored round, or a gradient. Images read with D3DX11 (from memory, as the texture pack's) into a texture with mips. |
| Filmtweaks | The Look tab's Filmtweaks on the map's own colour grade: the refdef's grade (refdef + 0x90) changed just before the renderer copies it (0x72B7B0) and put back after, each range's colour matrix the editor's after the map's: the Shadows, Midtones and Highlights tabs each have their own Contrast, Saturation, Hue, Temperature, Bleach and Tint (All moves the three together), with Brightness and Invert over all of them. Where the ranges meet stays the map's (the shader's `vc_RS`/`vc_RE` aren't worked out yet), so the edge rows aren't shown. At their defaults the picture is the map's. (`r_filmUseTweaks` with the `vc_` dvars made it black) |
| ReShade | When it's in the game, its effects are on the game's picture and in recordings, and the editor is drawn after them, without them (its present: the frame captured after its effects, the UI after its overlay). Its settings stay in its own menu |
| Cursor | While the editor is shown the game's ShowCursor calls (its import) don't reach Windows, so the game neither hides the cursor nor runs its count back in a loop |
| Escape | With the editor hidden, Escape opens the theater's menu |

## Not yet

- The bone camera follows players only (not other entities or the scene's actors).
- The actors' rings and paths; anything drawn with depth.
- Recording passes and a render size other than the window's.
- Playing another film from the Demo tab while one plays (leave it for the lobby first).

## Building

Run `BUILD-T6IRIS.bat` (which runs `build-t6.ps1`), or from an x86 Native Tools prompt with the DirectX SDK (June 2010)
and `DXSDK_DIR` set:

```
MSBuild IWXMVM.sln /t:t6 /p:Configuration=Debug /p:Platform=x86 /p:BO2Dir="F:\Games\bo2_game_files_test"
```

`/p:BO2Dir` copies `T6iris.Red32n` into that game's `Plugins` folder after the build (close the game first).

The D3D11 path needs `d3dx11_43.dll` from the DirectX June 2010 runtime and `d3dcompiler_47.dll` (part of Windows 10
and later). Recording needs `ffmpeg.exe` where the Iris Studio launcher keeps it (`%LOCALAPPDATA%\IrisStudio`).

## Files

| File | What it does |
| --- | --- |
| `src/Entrypoint.cpp`, `src/T6iris.def` | The nine functions Redacted's loader (`steam_api.dll`) looks up; `PreInit` starts the mod in `t6mp*.exe` only |
| `src/T6Interface.hpp` | Core's game interface for T6 |
| `src/Signatures.hpp`, `src/Addresses.hpp` | The game's functions and globals, by signature |
| `src/Theater.cpp` | The film: its file and length, seeking, the pause, recording's frame time, the theater's HUD flags |
| `src/Camera.cpp` | The editor's cameras and the theater's camera mode |
| `src/Functions.cpp`, `src/Patches.hpp`, `src/Structures.hpp` | Dvars, the console, keys; the input patch; the game's structures |
| `src/TheaterProbe.cpp` | A diagnostic that logs the theater's fields once a second, only with the environment variable `T6IRIS_PROBE` set |
| `hello/` | A standalone plugin that only logs Redacted's loader calls |

Core gained a D3D11 path for T6 (`core/src/D3D11.cpp`, `Renderer.cpp`), chosen per game by
`GameInterface::GetGraphicsApi()`; the D3D9 games are unchanged.
