# Testing a game

How to try opendlssg-fg on a game, and what a working run looks like in the log.

## Set up

1. Install as described in the [README](../README.md#installing).
2. In `opendlssg.ini`, set `Level=3` under `[Logging]` and
   `StreamlineDiagnostics=1` under `[Debug]`. See
   [CONFIGURATION.md](CONFIGURATION.md).
3. Start the game, turn on DLSS Frame Generation, and play for a minute.

Set `Level` back to `1` afterwards; level 3 logs every call.

Logs are in `opendlssg\logs` next to the executable: `loader_<pid>.jsonl` from
this project and `sl.log` from Streamline. Helper processes may write their own
`loader_<pid>.jsonl`; read the one whose first line has the game's executable as
`host`.

## A working run

The log contains, in order:

| Event | Meaning |
|---|---|
| `attach` with `config_found: true` | The DLL loaded and found its INI |
| `configuration` | The settings in force |
| `driver` | NVIDIA driver version and branch |
| `proxy_bound` with `resolved` equal to `total` | The system DLL is forwarded |
| `hook_installed` for `NvAPI_GPU_GetArchInfo` | The architecture hook is in place |
| `gpu_architecture` with `supported: true` | An RTX 30 card was detected |
| `arch_spoof_applied` | The gates are being opened |
| `multi_frame_unlocked` with `patched` equal to `gates` and `at_load: true` | The runtime may generate more than one frame |
| `provider_found` | A frame-generation runtime and its version, one line per runtime |
| `kernel_target_resolved` with `sm: 86` | The architecture kernels are built for |
| `dlssg_set_options` with `mode: on` | The game turned frame generation on |
| `kernel_substituted` with `method` `native` or `retarget` | Kernels were supplied; with multi-frame, retargets show `from_sm: 120` |
| `dlssg_state` with `presented` above 1 | The multiplier on screen, up to 6 |
| `kernels_summary` with `refused: 0` | Totals, written once the count settles |

At the default `Level=1`, only `attach`, `configuration`, `driver`,
`gpu_architecture`, `arch_spoof_applied`, `provider_found` and
`kernels_summary` appear, plus anything that failed. The other rows are written
at level 2, and each has an error or warning form that shows at level 1.

`presented` is read only when the game calls `slDLSSGSetOptions`, and reports
the previous call. A multiplier is confirmed only if the game applies its
options twice while it is selected. To capture it, select the multiplier, then
change another graphics setting or leave and re-enter the menu.

In `sl.log`, expect `adapter mask 0x1` for `sl.dlss_g` and no `DLSS-G cannot
run`.

## When it does not work

| Symptom | Where to look |
|---|---|
| No `opendlssg\logs` folder | The game does not load this proxy name; try another |
| Logs only from a launcher | `host` is a launcher; move the DLL next to the real executable (Unreal Engine: `Binaries\Win64`) |
| No frame generation option | `sl.log` for `adapter mask 0x0` or `DLSS-G cannot run`; `hook_failed` in ours |
| No option, `sl.log` says the OS disabled it | `hardware_scheduling` with `enabled: false`. Logged only when hardware-accelerated GPU scheduling differs from the Windows default |
| No option in a game from before 2024 | Streamline 1.x, which is not supported: `hook_export_missing` for `slGetFeatureFunction`, and `patchprobe` finds no patch sites |
| No 3x or 4x option | `multi_frame_gates_not_found`, or the game's plugin caps it (`SL Plugin supports N` in `sl.log`) |
| Option present, no extra frames | `kernel_substituted` missing or `kernel_refused` present |
| Frame generation on, nothing generated, no refusal | `cu_function_missing` names a kernel missing from the substituted module |
| Frame generation lowers the frame rate, Vulkan game | Check `reflex_present_pacing` with `low_latency_off: true` is logged. If not, and `PatchFlipMetering=1`, the driver may be pacing every present; see [ARCHITECTURE.md](ARCHITECTURE.md#reflex-and-present-pacing) |
| `StreamlineDiagnostics=1` but no `sl.log` | The proxy loaded after Streamline started; set the variables yourself, see [ARCHITECTURE.md](ARCHITECTURE.md#diagnosing-a-problem) |
| `NvAPI_D3D12_CreateCuModule failed` in `sl.log`, then a crash | The fatbin route failed; look for `cu_module_intercepted` |
| Vulkan game, no extra frames | `vulkan_hooks_unavailable` |
| `kernel_fallback` | The driver rejected a Blackwell kernel; 3x and above may be wrong, 2x still works |
| `kernel_refused`: `this runtime was not indexed` | Kernels came from a runtime the loader did not find; `caller` names it |
| `kernel_refused`: `the calling module could not be identified` | The call came through another tool's hook while several runtimes were loaded |
| `provider_pin_failed` | A runtime could not be kept loaded, so it was not indexed |
| Black screen or freeze | Windows System event log, `nvlddmkm`, `Restarting TDR occurred`: a GPU hang |
| Game stops responding after changing settings | `sl.log` for `PFunResizeBuffersBefore failed`, then `Pacer flush has timed out`: a swapchain resize failed. Seen in Crimson Desert, with all kernels supplied |
| A setting seems ignored | `config_value_rejected` |

## The DLSS override

*DLSS Override* in the NVIDIA app, or `DLSS-FG - Enable DLSS Override` in NVIDIA
Profile Inspector, makes NGX use its own runtime instead of the game's. No
setting is needed here. It is global, so it updates every game at once;
`[Runtime] Mode=Bundled` does one game.

That runtime is stored in
`%ProgramData%\NVIDIA\NGX\models\dlssg\versions\<build>\files` as
`<architecture>_<application id>.bin`; `nvngx_config.txt` there lists the
version per application. When it is in use, a `provider_found` path points into
that folder and `sl.log` reports the same `ngxFeatureVersion`. With both the
override and `Mode=Bundled`, the bundled runtime wins.

Expect three `provider_found` lines with the override on: the game's runtime,
the override's, and an older copy from the driver, which may log
`multi_frame_gates_not_found`. The one generating frames is the `caller` of
`kernel_substituted`.

Verified on an RTX 3080, driver 616.92. Crimson Desert (Direct3D 12) ran 310.9.0
from the override with 704 kernels supplied, none refused, `presented` 2, 3, 4
and 6. DOOM The Dark Ages (Vulkan) reached `presented: 6` on the override
runtime, which its shipped runtime did not.

Streamline can also run a plugin NVIDIA downloaded instead of the game's. It
lives under `%ProgramData%\NVIDIA\NGX\models`, and `sl.log` names it as
`Found plugin: ...\<architecture>_<application id>.dll`. The `plugin` field of
`flip_metering_forced` says which copy was patched.

## Reporting

Include the game, its store, the GPU, the driver version and both log files. A
`provider_untested` line means nobody has reported on that runtime version yet;
say whether it worked.

**Before posting a log publicly:** ours records install paths, the GPU, the
driver and the game's frame-generation settings. Paths under your Windows
profile are written as `%USERPROFILE%`; other paths are kept as they are.
`sl.log` is NVIDIA's and is not redacted; read it before attaching it.

## A newer runtime in an older game

`[Runtime] Mode=Bundled` loads a runtime of your choice in place of the game's,
without changing files on disk. 310.9.1 was verified this way in PRAGMATA and
Crimson Desert. In PRAGMATA it also removes the ghosting seen with path tracing
and Ray Reconstruction on the shipped 310.3.0.

## Tested so far

All on an RTX 3080.

| Game | API | Runtime | Result |
|---|---|---|---|
| PRAGMATA | Direct3D 12 | 310.3.0, 310.9.0, 310.9.1 | Shipped 310.3.0: 2x and 4x. 310.9.1 via `Mode=Bundled`: 2x, 3x and 4x, no ghosting with path tracing and Ray Reconstruction. 310.9.0 via the DLSS override, with ReShade, renodx and ReFramework loaded: `presented` 3 and 4, 64 kernels, none refused |
| DOOM The Dark Ages | Vulkan | 310.6.0, 310.9.0 | Shipped 310.6.0: `presented` 2, 3 and 4, 90 to 130 fps at 2x; 6x accepted but not confirmed. 310.9.0 via the DLSS override: `presented` 3 and 6, 192 kernels, none refused |
| Far Far West | Direct3D 12 | 310.6.0 | 2x. Menu has only on and off; 4x with `ForceMultiplier=4`, `presented: 4` |
| Halo Campaign Evolved | Direct3D 12 | 310.2.1 | 2x. Menu has only on and off; 4x with `ForceMultiplier=4`, `presented: 4`. Runs plugins NGX downloaded |
| Jurassic World Evolution 3 | Direct3D 12 | 310.3.0 | `presented` 2, 3 and 4 |
| Indiana Jones and the Great Circle | Vulkan | 310.2.1 | 2x, 3x and 4x with path tracing, `presented: 3` captured. Runs a plugin NGX downloaded. The game requests `numFramesToGenerate` 0 when switching modes, which Streamline rejects; this is passed through unchanged |
| Frostpunk 2 | Direct3D 12 | 310.5.2 | `presented` 2, 3 and 4. Artefacts in a cutscene at 3x, where `sl.log` shows the game's own frames arriving over 100 ms apart |
| Crimson Desert | Direct3D 12 | 310.9.1 | Via `Mode=Bundled`, with Ray Reconstruction: `presented` 2, 3, 4, 5 and 6. The only tested game whose plugin (Streamline 2.11.1) allows 5 generated frames |
| Corsair Cove | Direct3D 12 | 310.5.2 | `presented` 2 and 3; 4x accepted but not confirmed. Streamline 2.10.3, plugins NGX downloaded |
| No Man's Sky (Microsoft Store) | Vulkan | 310.9.0 | Via the DLSS override (the game ships 310.7.0). `presented` 2, 3 and 4, 192 kernels, none refused. Streamline ran NGX plugin 134656. Needs the [Reflex pacing fix](ARCHITECTURE.md#reflex-and-present-pacing): without it, 2x dropped output from about 136 fps to 59 |
| Palworld (Steam) | Direct3D 12 | 310.4.0 | `presented` 2, 3 and 4, 384 kernels, none refused. Needs the `-dx12` launch option (Direct3D 11 is the default and unsupported). Install the proxy as `dxgi.dll`; the game does not load `version.dll` |
