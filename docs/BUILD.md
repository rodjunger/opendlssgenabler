# Building

The artifacts are Windows x64 DLLs. They are cross-compiled from Linux or WSL
with LLVM-MinGW, which is the toolchain every change so far has been built and
tested with.

## Get the source

MinHook, libhat and LZ4 are submodules:

```bash
git clone --recursive https://github.com/rodjunger/opendlssg
# or, in an existing checkout:
git submodule update --init --recursive
```

## Toolchain

Download an LLVM-MinGW release for your host from
[mstorsjo/llvm-mingw](https://github.com/mstorsjo/llvm-mingw/releases), for
example `llvm-mingw-<date>-ucrt-ubuntu-22.04-x86_64.tar.xz`, and unpack it
anywhere. No root access is needed. CMake 3.21 or newer and Ninja are the only
other requirements.

```bash
export PATH="$HOME/opt/llvm-mingw-<date>-ucrt-ubuntu-22.04-x86_64/bin:$PATH"
```

## Build

```bash
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w64.cmake
cmake --build build
```

The proxy DLLs land in `build/src/`: `version.dll`, `winmm.dll`, `dinput8.dll`
and `dxgi.dll`. They are the same engine under the names games commonly load.
Each depends only on `KERNEL32` and the Universal C Runtime, which ships with
Windows 10 and 11. The build treats a new compiler warning as something to fix;
it currently produces none under `-Wall -Wextra`.

## Tests and tools

```bash
cmake -B build -DODG_BUILD_TESTS=ON -DODG_BUILD_TOOLS=ON
cmake --build build
```

- `build/odg_unit_tests.exe` checks the pure logic: configuration parsing, the
  shipped `opendlssg.ini`, fatbin, cubin and PTX handling, the NVAPI parameter
  block search, x86 decoding, the CUDA compatibility rule, UTF-8 conversion and
  log pruning. Run it on Windows; it needs no GPU. From WSL:
  `cmd.exe /c build\\odg_unit_tests.exe` from a Windows path.
- `build/ptxprobe.exe <nvngx_dlssg.dll> [target_sm] [newest]` checks, against the
  installed driver and without a game, that every PTX kernel in a runtime
  retargets and compiles for this GPU. `newest` checks the sources multi-frame
  generation uses. Use it on a runtime version nobody has tested yet.
- `build/patchprobe.exe <dll>...` reports what the patches would change, without a
  game: for `sl.dlss_g.dll` the flip-metering flag, the writes that would be
  patched and the frame-count clamp; for `nvngx_dlssg.dll` the multi-frame gates.
  Use it on a build nobody has tested yet.

## Proxy stubs

`src/proxy/generated/` holds the export-forwarding stubs for each proxy name,
generated from the system DLL's export table with Python and `pefile`:

```bash
pip install pefile
python3 tools/gen_proxy.py version /mnt/c/Windows/System32/version.dll
```

Regenerate them only to add a proxy name.

## Native Windows

MSVC is not supported, and configuring with it stops with an error. The generated
proxy stubs are GCC and Clang inline assembly, and LLVM-MinGW is the one
toolchain the project is built and tested with, from Linux or WSL as above.
