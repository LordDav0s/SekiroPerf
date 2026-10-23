# SekiroPerf

[![Build](https://github.com/LordDav0s/SekiroPerf/actions/workflows/build.yml/badge.svg)](https://github.com/LordDav0s/SekiroPerf/actions/workflows/build.yml)

A DLL mod for Sekiro: Shadows Die Twice that raises frame rate in two ways the game's settings can't:

1. **FSR 1.0 upscaling.** The game renders at a lower internal resolution, and every frame is upscaled
   to your monitor's native resolution with AMD FidelityFX Super Resolution 1.0 (edge-adaptive upscale
   plus contrast-adaptive sharpening). Shading cost scales with pixel count, so rendering 1080p for a
   1440p screen shades 56% of the pixels. Expect a large gain when your GPU is the bottleneck, with
   an image much closer to native than simply lowering the resolution.
2. **No 60 FPS cap.** Removes Sekiro's frame limit (with the movement-speed fix so running speed stays
   correct), lifts the forced 60 Hz in exclusive fullscreen, and can turn off the forced VSync.

Works with [Sekiro Mod Engine](https://www.nexusmods.com/sekiro/mods/6), or standalone.

## Install

Download the zip from the [latest release](https://github.com/LordDav0s/SekiroPerf/releases/latest)
and extract it. The files you need are `sekiro_perf.dll` and `sekiro_perf.ini`.

### With Mod Engine

1. Copy `sekiro_perf.dll` and `sekiro_perf.ini` into the game folder (next to `sekiro.exe`).
2. Open `modengine.ini` and set:
   ```ini
   chainDInput8DLLPath="\sekiro_perf.dll"
   ```
   The leading backslash matters: Mod Engine appends this to the game folder path.
3. Start the game.

If another mod is already in `chainDInput8DLLPath`, put SekiroPerf there and set that mod's DLL as
`ChainDLL` in `sekiro_perf.ini`.

### Without Mod Engine

Copy both files into the game folder and rename `sekiro_perf.dll` to `dinput8.dll`.

### Linux (Proton)

Same files, plus this Steam launch option (Mod Engine needs it too):

```
WINEDLLOVERRIDES="dinput8=n,b" %command%
```

## Getting the FPS

**Upscaling only does something when the in-game resolution is below your monitor's native
resolution.** Lower it in the game's graphics settings. Screen mode doesn't matter: the mod
runs the game as borderless fullscreen and handles "fullscreen" itself.

| Monitor   | In-game resolution | Pixels shaded | FSR mode (approx.) |
|-----------|--------------------|---------------|--------------------|
| 1920x1080 | 1600x900           | 69%           | Ultra Quality      |
| 1920x1080 | 1280x720           | 44%           | Quality            |
| 2560x1440 | 1920x1080          | 56%           | Ultra Quality      |
| 2560x1440 | 1600x900           | 39%           | Balanced           |
| 3840x2160 | 2560x1440          | 44%           | Quality            |
| 3840x2160 | 1920x1080          | 25%           | Performance        |

The FPS gain is roughly in line with the pixel reduction when the GPU is the bottleneck, and smaller
when the CPU is. Start with the first row for your monitor; drop to the second if you need more.

## Checking it works

`sekiro_perf.log` (next to the DLL) is rewritten every launch. A working run includes lines like:

```
DXGI: hooked IDXGIFactory::CreateSwapChain.
FSR: swap chain wrapped. Game renders at 1920x1080, sharpness 0.80.
FSR: output 2560x1440, game renders 1920x1080 (FSR upscaling).
Frame rate: 60 FPS cap removed, now capped at 144 FPS.
```

## Settings

Everything is in `sekiro_perf.ini`, with comments. The main ones:

| Setting | Default | What it does |
|---|---|---|
| `[Upscaling] Enable` | 1 | FSR upscaling from the in-game resolution to native. |
| `[Upscaling] Sharpness` | 0.8 | 0.0 (subtle) to 1.0 (strongest). |
| `[FrameRate] UnlockFps` | 1 | Remove the 60 FPS cap. |
| `[FrameRate] TargetFps` | 0 | New cap, 30–300. 0 = your refresh rate. |
| `[Display] DisableVSync` | 0 | For G-Sync/FreeSync: set 1, and TargetFps a few below refresh (e.g. 141 at 144 Hz). |
| `[Display] ForceHighestRefreshRate` | 1 | Fullscreen at your monitor's max refresh instead of 60 Hz (only with upscaling off). |

On a 60 Hz monitor the FPS cap doesn't matter (VSync holds you at 60) unless you also set
`DisableVSync=1` and a higher `TargetFps`; upscaling still helps if you were below 60.

## Troubleshooting

- **Black screen, crash, or garbled image:** set `[Upscaling] Enable=0` and open an issue with
  `sekiro_perf.log` attached. The FPS unlock works independently.
- **No `FSR: swap chain wrapped` line:** check the log for an `FSR:` error. On Linux, if it says
  shader compilation failed, install Microsoft's compiler: `protontricks 814380 d3dcompiler_47`.
- **Image cropped or wrong after changing resolution in the menu:** restart the game. The resolution
  the game starts with is handled reliably; some mid-game changes may not be.
- **Blurry on a desktop with display scaling (125%, 150%...):** sekiro.exe → Properties →
  Compatibility → Change high DPI settings → Override high DPI scaling → Application.
- **`Frame rate: gave up after 120 s`:** unsupported game version, or another FPS unlocker (e.g.
  SekiroFpsUnlockAndMore's frame limit option) already patched the same code. Use only one.

## Limitations

- Not tested in-game yet. The memory signatures are the ones SekiroFpsUnlockAndMore uses for
  sekiro.exe 1.2.0.0–1.5.0.0 (game patches 1.02–1.06) and the portable parts are unit-tested, but
  the upscaler hasn't been run in Sekiro yet. Expect rough edges until it has.
- The whole frame is upscaled, HUD included, so text is slightly softer than native.
- With a lowered in-game resolution, the mouse cursor in menus may not line up with what's on screen.
  Gameplay and controllers are unaffected.
- The upscaler needs Direct3D 11.1 (Windows 8+, or Proton) and `d3dcompiler_47.dll` (ships with Windows 10+).

## How it works

- **Loading:** Mod Engine `LoadLibrary`s the DLL from its own `DllMain` and forwards the game's
  `DirectInput8Create` to it; SekiroPerf forwards that on to the system `dinput8.dll` (or `ChainDLL`).
- **Swap chain hooks:** a worker thread patches the shared vtable of `IDXGIFactory::CreateSwapChain` /
  `IDXGIFactory2::CreateSwapChainForHwnd` before the game's renderer starts.
- **Upscaling:** the game gets a proxy `IDXGISwapChain4`. `GetBuffer(0)` returns a texture at the
  game's resolution; on `Present` the proxy runs EASU into an FP16 intermediate, then RCAS into the
  real back buffer (sized to the window), inside its own `ID3DDeviceContextState`, so the game's
  pipeline state is untouched. Fullscreen is virtual; the real window is borderless at monitor size.
- **FPS unlock:** sekiro.exe is DRM-wrapped, so the worker rescans its code until it unpacks, then
  rewrites the frame-time immediate (`mov [rbx+18h], 1/60`) and points the movement-speed constant's
  RIP-relative operand at a value matching the new frame rate. A watchdog re-applies both if reverted.

## Building

Cross-compile on Linux/macOS/MSYS2 with MinGW-w64:

```
make            # build/sekiro_perf.dll
make test       # host-side unit tests (signature scanner, patch math, FSR constants)
make dist       # dist/SekiroPerf/ with the DLL, ini and docs
```

Or on Windows with CMake and MSVC:

```
cmake -B build -A x64
cmake --build build --config Release
```

## Credits

- Frame-lock and speed-fix signatures: [SekiroFpsUnlockAndMore](https://github.com/uberhalit/SekiroFpsUnlockAndMore) by uberhalit (MIT).
- FSR 1.0: [AMD FidelityFX Super Resolution](https://github.com/GPUOpen-Effects/FidelityFX-FSR) (MIT).

See `THIRD_PARTY_NOTICES.md`.
