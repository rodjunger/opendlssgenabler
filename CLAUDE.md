# Working notes for Claude

opendlssg-fg enables NVIDIA DLSS Frame Generation on RTX 30 (Ampere), in games
that already ship NVIDIA's frame generation. It is an enabler, not an
implementation: the frames come from the game's own NVIDIA runtime, and this
project only removes the restrictions that stop that runtime running on an
older card. GPLv3, a clean-room reimplementation of the enabler
`sdli1995/dlssg_for_sm86`, not of DLSS-G itself. It is a proxy DLL (`version`,
`winmm`, `dinput8` or `dxgi`) that patches NVIDIA's own components in memory
only.

`docs/ARCHITECTURE.md` is the reference for how and why it works. Read it before
changing behaviour; keep it true when behaviour changes.

## How to work here

- **Quality first.** This is meant to run on thousands of machines. Code should
  read like a senior engineer wrote it: clear names, small functions, one purpose
  each, no dead paths, no speculative abstraction. Prefer deleting over adding.
- **Reuse libraries.** Established libraries beat hand-written parsing or byte
  matching: libhat for signature scanning, LZ4 for decompression, HDE64 (bundled
  with MinHook) for instruction decoding. Look for one before writing your own.
- **No magic numbers.** Every constant is named and its meaning stated. Structure
  layouts are declared as structs with a source (CUDA `fatbinary.h`, the System V
  ELF ABI, the NVAPI headers), never as bare offsets.
- **Find code by pattern, never by address.** Signatures and decoded instructions,
  confirmed and bounded, so a new build of someone else's binary either matches or
  is refused. Anchor on something meaningful: a string the code references, a
  parameter name it publishes.
- **Fail closed.** If something cannot be verified, change nothing and log why.
  A game that loses frame generation is acceptable; a game that hangs the GPU or
  runs code the hardware cannot execute is not.
- **Comments say why, not what.** Keep them where the reasoning is not obvious,
  especially around other people's binaries. No filler.
- **Documentation is short, professional and current.** No em dashes anywhere.
- **Verify before claiming.** Run the offline tools or read the logs, and say
  plainly what was tested and what was not. Never present an expectation as a
  result.

## Build and check

```bash
export PATH=~/opt/llvm-mingw-20260908-ucrt-ubuntu-22.04-x86_64/bin:$PATH
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w64.cmake \
      -DODG_BUILD_TESTS=ON -DODG_BUILD_TOOLS=ON
cmake --build build          # zero warnings is the standard
```

Windows binaries run from WSL through `cmd.exe /c`, from a path without spaces
(copy to a folder under `/mnt/c/Users/<you>/` first):

- `odg_unit_tests.exe` needs `assets/opendlssg.ini` beside it. All checks must pass.
- `patchprobe.exe <sl.dlss_g.dll|nvngx_dlssg.dll>...` prints the patch sites a
  build would use, without a game. Run it on any new NVIDIA build.
- `ptxprobe.exe <nvngx_dlssg.dll> [sm] [newest]` compiles every kernel against the
  installed driver. `newest` checks the sources multi-frame uses.

Prefer these over launching a game. They answer most questions in seconds.

## Testing in a game

Ask before launching or closing a game; the user may be using the machine. Always
check `cmd.exe /c tasklist` first, and close what you started. Installed test
games and their quirks are in `docs/TESTING.md`.

The proxy goes beside the executable that actually renders. In Unreal Engine that
is `<Game>\Binaries\Win64`, not the launcher in the install folder; a log whose
`host` is a launcher means the DLL is in the wrong place.

Read our `loader_<pid>.jsonl` with targeted greps. `sl.log` (Streamline's own log,
enabled by `StreamlineDiagnostics=1`) reaches several MB: never read it whole,
grep it with timestamps and numbers stripped and `sort -u`.

## Invariants worth keeping

- The architecture spoof is **caller-scoped**. Telling every caller Ada removes
  the device. Only `sl.common.dll`, `_nvngx.dll` and `nvngx_dlssg.dll` are told.
  A caller is the component it is, not the file name it carries: NGX downloads
  replacements under `%ProgramData%\NVIDIA\NGX\models`, and those run instead
  of the copies a game ships.
- **Never install an inline hook under the loader lock.** Patching bytes there is
  fine and is how the runtime is patched before its code runs.
- Hooks are installed through `hooks::Install`, which publishes the trampoline
  before enabling. The proxy pins itself so its code cannot be unloaded.
- Kernel images are only ever replaced by something the GPU can run: NVIDIA's own
  Ampere cubin, or PTX retargeted from the runtime's own container. Nothing is
  shipped or stored.
- `log::Header` is for lines that identify a run and must survive an errors-only
  log: the process, the settings, the GPU, the driver, the runtime, the kernel
  totals. `Level::Error` means the engine failed at something it set out to do;
  `Level::Warning` means it carried on, including every deliberate refusal;
  `Level::Info` is one line per decision; `Level::Trace` is per call, per kernel,
  per frame, and must be deduplicated before it goes in a hot path.
