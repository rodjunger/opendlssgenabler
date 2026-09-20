# Third-party notices

This project is GPLv3. It uses the following components under their own terms.

## Bundled as source or submodule

- **MinHook** (`third_party/minhook`), Tsuda Kageyu. BSD 2-Clause. Inline hook
  installation, and its bundled HDE64 decoder (Vyacheslav Patkov, BSD 2-Clause)
  for reading instructions.
- **libhat** (`third_party/libhat`), Brady Hahn. MIT. Signature scanning.
- **LZ4** (`third_party/lz4`), Yann Collet. BSD 2-Clause (`lib/`). Decompressing
  fatbin payloads.
- **NVIDIA Streamline public headers** (`third_party/streamline/include`),
  NVIDIA Corporation. MIT, see `third_party/streamline/LICENSE.txt`. Used to
  call the Streamline interposer with the correct ABI.

## Not bundled

- **NVIDIA NVAPI open headers**, NVIDIA Corporation, MIT. The architecture ids
  and interface ids this project uses are reproduced as named constants in
  `src/spoof/nvapi.cpp` rather than by including the headers.

## Derivation

The proxy and NVAPI architecture-reporting design is derived from
`Nukem9/dlssg-to-fsr3`, GPLv3. The multi-frame gate and pacing references are
`dashdogy/RTX40MFG-Unlock` and `danzig666/RTX40MFG-minimal`, MIT.

## Not included

NVIDIA's `nvngx_dlssg.dll`, the NGX runtime, and all NVIDIA compute kernels are
the property of NVIDIA Corporation. They are not part of this repository, are
not redistributed in any release, and are not relicensed here.

This project ships no kernels of any kind. It rewrites the architecture
directive of the PTX the user's own installed runtime hands to the driver, in
memory, for the duration of the call. Nothing is extracted, stored, or derived
offline.
