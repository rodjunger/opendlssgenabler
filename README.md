# opendlssg-fg

Enables NVIDIA DLSS Frame Generation on GeForce RTX 30 (Ampere) cards, in
Windows games that already support it.

It does not generate frames itself. The game's own copy of NVIDIA's frame
generation does that. NVIDIA limits it to RTX 40 and 50 in two ways, and this
project removes both, in memory, while the game runs:

- the software refuses to offer the feature on older cards;
- the GPU code it sends to the driver does not run on RTX 30.

This means:

- It only works in games that already have DLSS Frame Generation.
- No NVIDIA files are included or modified.
- Image quality and frame pacing are NVIDIA's.

> **Status: early development.** Works on an RTX 3080 in all 11 games tested so
> far, at up to 6x where the game allows it. See the
> [list of tested games](docs/TESTING.md#tested-so-far). Some games will not
> work yet; reports help.

| GPU | Support |
|---|---|
| RTX 30 (Ampere) | Supported |
| RTX 40, 50 | Not needed; left untouched |
| RTX 20 (Turing) | Not supported yet; left untouched ([why](docs/ARCHITECTURE.md#scope-and-safety)) |

## Installing

1. Close the game.
2. Download the latest release, or [build it](docs/BUILD.md).
3. Copy `version.dll` and `opendlssg.ini` next to the game's executable. Back up
   any existing `version.dll` first. In Unreal Engine games this is
   `<Game>\Binaries\Win64`, not the launcher in the install folder.
4. Start the game and turn on DLSS Frame Generation in its graphics settings.

The default settings work in most games. See
[CONFIGURATION.md](docs/CONFIGURATION.md) for the exceptions.

**No `opendlssg\logs` folder next to the executable?** The game does not load
`version.dll`. Remove it and try `winmm.dll`, `dinput8.dll` or `dxgi.dll`
instead, one at a time.

**Requirement:** hardware-accelerated GPU scheduling must be on (Windows
Settings > System > Display > Graphics).

**To uninstall,** delete the DLL and `opendlssg.ini`.

The NVIDIA app's *DLSS Override* works alongside this project with no extra
setup.

## How it works

- **Unlocking the feature.** Streamline, NVIDIA's NGX and the frame-generation
  runtime each check the GPU. Only those three are told it is an RTX 40; the
  game and everything else see the real GPU.
- **Frame pacing.** RTX 30 lacks the hardware pacing RTX 40 uses, so the
  frame-generation plugin is switched to the software pacing it already has.
- **GPU code.** Each piece of RTX 40 code is replaced with NVIDIA's own RTX 30
  version where the runtime includes one, or otherwise recompiled for this GPU.
  Code that cannot be made to run is refused rather than risk a GPU hang.
- **3x and above.** The runtime reserves these for RTX 50. That check is lifted,
  and the matching GPU code is built from the RTX 50 version.

Details are in [ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Reporting a problem

In `opendlssg.ini`, set `Level=3` under `[Logging]` and
`StreamlineDiagnostics=1` under `[Debug]`. Reproduce the problem and attach the
files from `opendlssg\logs`. [TESTING.md](docs/TESTING.md#reporting) says what
to include.

## Documentation

- [How it works](docs/ARCHITECTURE.md)
- [Configuration](docs/CONFIGURATION.md)
- [Building](docs/BUILD.md)
- [Testing a game](docs/TESTING.md)

## License

GPLv3. See [LICENSE](LICENSE) and, for bundled components,
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

NVIDIA DLSS, DLSS-G, NGX, NVAPI and Streamline are the property of NVIDIA
Corporation. This project does not include or relicense them.

## Credits

- [sdli1995/dlssg_for_sm86](https://github.com/sdli1995/dlssg_for_sm86): the
  closed-source original this project reimplements in the open.
- [Nukem9/dlssg-to-fsr3](https://github.com/Nukem9/dlssg-to-fsr3): the proxy and
  NVAPI approach.
- [dashdogy/RTX40MFG-Unlock](https://github.com/dashdogy/RTX40MFG-Unlock) and
  [danzig666/RTX40MFG-minimal](https://github.com/danzig666/RTX40MFG-minimal):
  the plugin patch technique.
- [MinHook](https://github.com/TsudaKageyu/minhook): inline hooking and x86
  decoding.
- [libhat](https://github.com/BasedInc/libhat): signature scanning.
- [LZ4](https://github.com/lz4/lz4): fatbin decompression.
