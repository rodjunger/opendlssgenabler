# opendlssg-fg

An open source enabler for NVIDIA DLSS Frame Generation on GeForce RTX 30
(Ampere), in Windows games that already ship it.

**This is not an implementation of frame generation.** It generates no frames
and contains no frame-generation code. The work is done by NVIDIA's own DLSS-G
runtime, the one the game already installs, running on your own driver.
NVIDIA restricts that runtime to RTX 40 and 50, in two ways: its software
refuses to offer the feature on an older card, and it hands the driver CUDA
kernels an RTX 30 cannot execute. opendlssg-fg undoes exactly those two
restrictions, in memory, in the game's own process.

What follows from that:

- It only works in a game that **already has** DLSS Frame Generation. It cannot
  add the feature to a game that does not ship it.
- Nothing of NVIDIA's is included, redistributed or modified on disk. The
  kernels an RTX 30 runs are built at load time from the ones the game's own
  runtime already contains.
- Image quality and pacing are NVIDIA's. This project decides nothing about how
  a generated frame looks.

> **Status: early development.** Frame generation works on an RTX 3080 in the
> nine games tested so far, up to 6x where the game and its Streamline plugin
> allow it. Expect games that do not work yet; reports help.

| Game | API | Result |
|---|---|---|
| PRAGMATA | Direct3D 12 | 2x to 4x work |
| DOOM The Dark Ages | Vulkan | 2x to 6x work |
| Indiana Jones and the Great Circle | Vulkan | 2x and 3x work |
| Jurassic World Evolution 3 | Direct3D 12 | 2x to 4x work |
| Corsair Cove | Direct3D 12 | 2x and 3x work |
| Frostpunk 2 | Direct3D 12 | 2x to 4x work |
| Crimson Desert | Direct3D 12 | 2x to 6x work |
| Far Far West | Direct3D 12 | 2x works; 4x with `ForceMultiplier` |
| Halo Campaign Evolved | Direct3D 12 | 2x works; 4x with `ForceMultiplier` |

| GPU | Support |
|---|---|
| RTX 30 (Ampere) | Supported |
| RTX 40, 50 | Not needed. Detected and left untouched |
| RTX 20 (Turing) | Not supported yet. Detected and left untouched ([why](docs/ARCHITECTURE.md#scope-and-safety)) |

## Installing

1. Close the game.
2. [Build](docs/BUILD.md) the DLL. Prebuilt releases will follow.
3. Copy `version.dll` and `opendlssg.ini` next to the game's executable. If a
   `version.dll` is already there, back it up first. In an Unreal Engine game
   that is `<Game>\Binaries\Win64`, beside `<Game>-Win64-Shipping.exe`, not the
   launcher in the install folder. Ship `opendlssg.ini` as it comes; its
   defaults are the ones to run, and
   [docs/CONFIGURATION.md](docs/CONFIGURATION.md) covers the few cases that
   need an edit.
4. Start the game and enable DLSS Frame Generation in its graphics settings.

If no `opendlssg\logs` folder appears next to the executable, the game does not
load `version.dll`. Remove it and try `winmm.dll`, then `dinput8.dll`, then
`dxgi.dll`, one at a time. To uninstall, delete the DLL and `opendlssg.ini`.

Hardware-accelerated GPU scheduling must be on (Windows Settings, System,
Display, Graphics). Streamline refuses frame generation without it.

*DLSS Override* in the NVIDIA app, and `DLSS-FG - Enable DLSS Override` in
NVIDIA Profile Inspector, make NGX run a frame-generation runtime of its own in
place of the one a game ships. The engine treats that runtime the same as any
other and needs nothing set here.

## How it works

NVIDIA withholds frame generation from RTX 30 twice over: the software refuses
to offer it, and the runtime has no kernels an RTX 30 can execute.

- **Opening the gates.** Streamline, the NGX core and the runtime each check the
  GPU architecture. The engine answers Ada to those three components only, early
  enough that Streamline's one-time check sees it. Everything else, the game
  included, sees the real GPU.
- **Pacing.** RTX 30 has no hardware flip metering, so the frame-generation plugin
  is steered onto the software pacing it already carries.
- **Kernels.** The runtime hands the driver kernels built for Ada. The engine
  replaces each one with NVIDIA's own Ampere build where the runtime carries one,
  and otherwise with its PTX retargeted to this GPU. Anything that cannot be made
  to run is refused instead of hanging the GPU.
- **Multi-frame.** The runtime reserves 3x and above for RTX 50. The engine moves
  that check to the architecture it reports, and builds those kernels from the
  RTX 50 PTX, which is the one written for more than one generated frame.

[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) explains each component, every change
the engine makes and where, and how to read the logs.

## Reporting a problem

Set `Level=3` under `[Logging]` and `StreamlineDiagnostics=1` under `[Debug]` in
`opendlssg.ini`, reproduce the problem, and attach the files from
`opendlssg\logs`. [docs/TESTING.md](docs/TESTING.md) lists what a healthy run looks
like, and [docs/CONFIGURATION.md](docs/CONFIGURATION.md) explains every setting.

## Documentation

- [How it works](docs/ARCHITECTURE.md)
- [Configuration](docs/CONFIGURATION.md)
- [Building](docs/BUILD.md)
- [Testing a game](docs/TESTING.md)

## License

GPLv3. See [LICENSE](LICENSE), and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)
for bundled components.

NVIDIA DLSS, DLSS-G, NGX, NVAPI and Streamline are the property of NVIDIA
Corporation. This project neither includes nor relicenses them.

## Credits

- [sdli1995/dlssg_for_sm86](https://github.com/sdli1995/dlssg_for_sm86), the
  closed-source project whose behaviour this reproduces in the open.
- [Nukem9/dlssg-to-fsr3](https://github.com/Nukem9/dlssg-to-fsr3), the proxy and
  NVAPI approach.
- [dashdogy/RTX40MFG-Unlock](https://github.com/dashdogy/RTX40MFG-Unlock) and
  [danzig666/RTX40MFG-minimal](https://github.com/danzig666/RTX40MFG-minimal), the
  frame-generation plugin patch technique.
- [MinHook](https://github.com/TsudaKageyu/minhook), inline hooking and x86
  decoding.
- [libhat](https://github.com/BasedInc/libhat), signature scanning.
- [LZ4](https://github.com/lz4/lz4), fatbin payload decompression.
