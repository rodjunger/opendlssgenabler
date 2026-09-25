# Building

The outputs are Windows x64 DLLs, cross-compiled from Linux or WSL with
LLVM-MinGW.

## Get the source

MinHook, libhat and LZ4 are submodules:

```bash
git clone --recursive https://github.com/rodjunger/opendlssg
# or, in an existing checkout:
git submodule update --init --recursive
```

## Toolchain

Download an LLVM-MinGW release from
[mstorsjo/llvm-mingw](https://github.com/mstorsjo/llvm-mingw/releases), for
example `llvm-mingw-<date>-ucrt-ubuntu-22.04-x86_64.tar.xz`, and unpack it
anywhere. You also need CMake 3.21 or newer and Ninja.

```bash
export PATH="$HOME/opt/llvm-mingw-<date>-ucrt-ubuntu-22.04-x86_64/bin:$PATH"
```

## Build

```bash
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w64.cmake
cmake --build build
```

The proxy DLLs are written to `build/src/`: `version.dll`, `winmm.dll`,
`dinput8.dll` and `dxgi.dll`. They are the same code under different names.
Each depends only on `KERNEL32` and the Universal C Runtime, which ships with
Windows 10 and 11. The build has no warnings under `-Wall -Wextra`; keep it that
way.

## Tests and tools

```bash
cmake -B build -DODG_BUILD_TESTS=ON -DODG_BUILD_TOOLS=ON
cmake --build build
```

All three run on Windows. From WSL, copy them to a Windows path and run them
with `cmd.exe /c`.

- `odg_unit_tests.exe` tests the pure logic: configuration parsing, the shipped
  `opendlssg.ini`, fatbin, cubin and PTX handling, the NVAPI parameter block
  search, x86 decoding, the CUDA compatibility rule, UTF-8 conversion, log
  pruning and the patch-site analysis. It needs no GPU.
- `ptxprobe.exe <nvngx_dlssg.dll> [target_sm] [newest]` compiles every PTX
  kernel in a runtime for this GPU, against the installed driver. `newest`
  checks the sources multi-frame generation uses.
- `patchprobe.exe <dll>...` prints what the patches would change. For
  `sl.dlss_g.dll`: the flip-metering flag, the writes to patch and the
  frame-count clamp. For `nvngx_dlssg.dll`: the multi-frame gates and the kernel
  index, including any image it cannot attribute.

Run `ptxprobe` and `patchprobe` on any NVIDIA build nobody has tested yet.

## Proxy stubs

`src/proxy/generated/` holds the export-forwarding stubs for each proxy name,
generated from the system DLL with Python and `pefile`. Regenerate them only to
add a proxy name:

```bash
pip install pefile
python3 tools/gen_proxy.py version /mnt/c/Windows/System32/version.dll
```

## Native Windows

MSVC is not supported; configuring with it fails. The proxy stubs use GCC and
Clang inline assembly.
