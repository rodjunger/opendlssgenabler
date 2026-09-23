# Working notes for Claude

opendlssg-fg enables NVIDIA DLSS Frame Generation on RTX 30 (Ampere) in games
that already ship it. It is an enabler, not an implementation: the game's own
NVIDIA runtime generates the frames, and this project removes the restrictions
that stop it running on Ampere. It is a proxy DLL (`version`, `winmm`,
`dinput8` or `dxgi`) that patches NVIDIA's components in memory only. GPLv3, a
clean-room reimplementation of the enabler `sdli1995/dlssg_for_sm86`, not of
DLSS-G.

`docs/ARCHITECTURE.md` explains how and why it works. Read it before changing
behaviour.

## How to work here

- **Quality first.** This runs on thousands of machines. Clear names, small
  single-purpose functions, no dead code, no speculative abstraction. Prefer
  deleting to adding.
- **Reuse libraries.** libhat for signature scanning, LZ4 for decompression,
  HDE64 (bundled with MinHook) for instruction decoding. Look for a library
  before writing a parser.
- **No magic numbers.** Name every constant and state its meaning. Declare
  structure layouts as structs with a source (CUDA `fatbinary.h`, the System V
  ELF ABI, the NVAPI headers), never as bare offsets.
- **Find code by pattern, never by address.** Use bounded, confirmed signatures
  and decoded instructions, so a new build of someone else's binary either
  matches or is refused. Anchor on something meaningful, such as a string the
  code references or a parameter name it publishes.
- **Fail closed.** If something cannot be verified, change nothing and log why.
  Losing frame generation is acceptable; hanging the GPU or running code it
  cannot execute is not.
- **Comments say why, not what.** Only where the reasoning is not obvious,
  especially around other people's binaries.
- **Verify before claiming.** Run the offline tools or read the logs. Say what
  was tested and what was not. Never present an expectation as a result.

## Documentation

- **Docs must always match the code.** Any change to behaviour, settings, log
  events, tools or tested games updates the affected docs in the same change.
  Outdated docs are a bug.
- **Short and plain.** Direct sentences, one idea each. Use tables and bullets
  for lists of facts. Cut filler, repetition and dramatic phrasing ("this is
  load-bearing", "not a refinement", "however correct it is").
- **Say each thing once.** Put a fact in the doc it belongs to and link to it
  from the others. The tested-games table lives only in `docs/TESTING.md`.
- **Write for the reader.** The README is for players: no internals. Internals
  go in `docs/ARCHITECTURE.md`.
- No em dashes anywhere.

## Build and check

```bash
export PATH=~/opt/llvm-mingw-20260908-ucrt-ubuntu-22.04-x86_64/bin:$PATH
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w64.cmake \
      -DODG_BUILD_TESTS=ON -DODG_BUILD_TOOLS=ON
cmake --build build          # must build with zero warnings
```

Run Windows binaries from WSL with `cmd.exe /c`, from a path without spaces
(copy them under `/mnt/c/Users/<you>/` first):

- `odg_unit_tests.exe` needs `assets/opendlssg.ini` beside it. All checks must
  pass.
- `patchprobe.exe <sl.dlss_g.dll|nvngx_dlssg.dll>...` prints the patch sites a
  build would use. Run it on any new NVIDIA build.
- `ptxprobe.exe <nvngx_dlssg.dll> [sm] [newest]` compiles every kernel against
  the installed driver. `newest` checks the sources multi-frame uses.

Prefer these to launching a game; they answer most questions in seconds.

## Testing in a game

Ask before launching or closing a game; the user may be using the machine.
Check `cmd.exe /c tasklist` first, and close what you started. Installed test
games and their quirks are in `docs/TESTING.md`.

The proxy goes beside the executable that renders. In Unreal Engine that is
`<Game>\Binaries\Win64`, not the launcher. A log whose `host` is a launcher
means the DLL is in the wrong place.

Read `loader_<pid>.jsonl` with targeted greps. `sl.log` (Streamline's log,
enabled by `StreamlineDiagnostics=1`) reaches several MB: never read it whole;
grep it with timestamps and numbers stripped, then `sort -u`.

## Invariants

- The architecture spoof is **caller-scoped**; telling every caller Ada removes
  the device. Only `sl.common.dll`, `_nvngx.dll` and `nvngx_dlssg.dll` are told.
  Callers are matched by component, not file name, because NGX downloads
  replacements under `%ProgramData%\NVIDIA\NGX\models` that run instead of the
  game's copies.
- **Never install an inline hook under the loader lock.** Patching bytes there
  is fine; that is how the runtime is patched before its code runs.
- Hooks go through `hooks::Install`, which publishes the trampoline before
  enabling. The proxy pins itself so its code cannot be unloaded.
- Kernel images are only replaced by something the GPU can run: NVIDIA's Ampere
  cubin, or PTX retargeted from the runtime's own container. Nothing is shipped
  or stored.
- Log levels:
  - `log::Header`: lines that identify a run and must appear even in an
    errors-only log (process, settings, GPU, driver, runtime, kernel totals).
  - `Level::Error`: the engine failed at something it set out to do.
  - `Level::Warning`: it carried on, including every deliberate refusal.
  - `Level::Info`: one line per decision.
  - `Level::Trace`: per call, kernel or frame; deduplicate before using it in a
    hot path.
