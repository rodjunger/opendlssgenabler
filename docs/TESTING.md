# Testing a game

How to try opendlssg-fg on a game, and what a healthy run looks like in the log.

## Set up

1. Install as described in the [README](../README.md#installing).
2. In `opendlssg.ini`, set `Level=3` under `[Logging]` and
   `StreamlineDiagnostics=1` under `[Debug]`. Every setting is described in
   [CONFIGURATION.md](CONFIGURATION.md).
3. Start the game, enable DLSS Frame Generation, and play for a minute.

Level 3 is for investigating a game, not for using one: it records every call
the game makes, which in Indiana Jones and the Great Circle is a quarter of a
million lines. The default, `Level=1`, keeps errors and warnings and still
names the GPU, the driver, the game, the runtime and the kernel totals, which
is what a bug report needs.

Logs appear in `opendlssg\logs` next to the executable: `loader_<pid>.jsonl`
from the engine, and `sl.log` from Streamline. Some games start helper
processes that load the proxy too, each with its own `loader_<pid>.jsonl`; the
one to read has the game's executable as `host` in its first line.

## A healthy run

In order, the engine's log contains:

| Event | Meaning |
|---|---|
| `attach` with `config_found: true` | The engine loaded and found its INI |
| `configuration` | The settings in force, including `force_multiplier` |
| `driver` | The NVIDIA driver version and branch |
| `proxy_bound` with `resolved` equal to `total` | The system DLL is forwarded |
| `hook_installed` for `NvAPI_GPU_GetArchInfo` | The architecture hook is in place |
| `gpu_architecture` with `supported: true` | An RTX 30 card was detected |
| `arch_spoof_applied` | The gates are being opened |
| `multi_frame_unlocked` with `patched` equal to `gates` and `at_load: true` | The runtime may generate more than one frame |
| `provider_found` | A frame-generation runtime and its version, one line per runtime mapped |
| `kernel_target_resolved` with `sm: 86` | The architecture kernels are supplied for |
| `dlssg_set_options` with `mode: on` | The game switched frame generation on |
| `kernel_substituted` with `method` `native` or `retarget` | Kernels were supplied; with multi-frame, retargets show `from_sm: 120` |
| `dlssg_state` with `presented` above 1 | The multiplier actually on screen, up to 6 where the game and its plugin allow it |

| `kernels_summary` with `refused: 0` | The totals, written once the count settles |

`presented` is read only when the game calls `slDLSSGSetOptions`, and it lags
that call by one, so a multiplier is confirmed only where the game re-applies
its options at least twice while it is in force. A multiplier selected and left
alone can therefore be accepted with `result: eOk` and never appear in any
`presented` line. To capture one, select it and then make the game re-apply,
for instance by changing another graphics setting or leaving and re-entering
the menu, without changing the multiplier itself.

Most of that table is written at `Level=2`. `Level=1`, the default, keeps the
lines that identify the run, `attach`, `configuration`, `driver`,
`gpu_architecture`, `arch_spoof_applied`, `provider_found` and
`kernels_summary`, and everything that went wrong. The rows not in that list,
such as `proxy_bound` or `multi_frame_unlocked`, confirm a healthy step and are
silent at `Level=1`, but each has an error or warning form that is not: a
default-level report still shows any step that failed.

Streamline's `sl.log` should contain `adapter mask 0x1` for `sl.dlss_g` and no
line saying `DLSS-G cannot run`.

## When it does not work

| Symptom | Where to look |
|---|---|
| No `opendlssg\logs` folder | The game does not load this proxy name; try another |
| Logs only from a launcher | `host` names a launcher, not the game; move the DLL next to the real executable (Unreal Engine: `Binaries\Win64`) |
| No frame generation option | `sl.log` for `adapter mask 0x0` or `DLSS-G cannot run`; `hook_failed` in ours |
| Option present, no extra frames | `kernel_substituted` missing or `kernel_refused` present |
| `NvAPI_D3D12_CreateCuModule failed` in `sl.log`, then a crash | The runtime loads its kernels as one fatbin and the hook for it is missing or refused the module; our log should show `cu_module_intercepted` |
| Frame generation on, nothing generated, no refusal | `cu_function_missing` names a kernel the runtime asked for and could not find in the substituted module |
| Black screen or freeze | Windows System event log, `nvlddmkm`, `Restarting TDR occurred`: a GPU hang |
| The game stops responding after a settings change | `sl.log` for `PFunResizeBuffersBefore failed`, then `Pacer flush has timed out` and `Wait on gpu fence timed out`. A swapchain resize failed and the frame the pacer was waiting for never arrived. Seen in Crimson Desert while changing settings repeatedly, with every kernel supplied and none refused, and seen without a hang in other runs of the same game |
| Setting seems ignored | `config_value_rejected` |
| No option, and `sl.log` says the OS disabled it | `hardware_scheduling` with `enabled: false`. The line is written only when hardware-accelerated GPU scheduling has been changed from the Windows default, so no line means it was never turned off |
| Vulkan game, no extra frames | `vulkan_hooks_unavailable`: the Vulkan loader was loaded but could not be hooked |
| No 3x or 4x option | `multi_frame_gates_not_found` names how many comparisons were found and how many were read as an ordering test; or the game's own plugin caps it, which `sl.log` states as `SL Plugin supports N` |
| `kernel_fallback` | The driver rejected a Blackwell kernel; 3x and above may be wrong, 2x is kept |
| `kernel_refused` saying `this runtime was not indexed` | Kernels arrived from a runtime the loader did not find. Its `caller` names it; `provider_found` says which runtimes were indexed |
| `kernel_refused` saying `the calling module could not be identified` | The call came through another tool's hook, and more than one runtime is indexed, so which one it belongs to cannot be told |
| `provider_pin_failed` | A runtime was found and could not be kept mapped, so it was not indexed |
| No option in a game from before 2024 | Its Streamline is 1.x, which this engine does not reach: `hook_export_missing` for `slGetFeatureFunction`, and `patchprobe` finds no patch sites. A runtime from `[Runtime] Mode=Bundled` loads and indexes, but the plugin still refuses |

## The DLSS override

*DLSS Override* in the NVIDIA app, and `DLSS-FG - Enable DLSS Override` in
NVIDIA Profile Inspector, make NGX run a frame-generation runtime of its own in
place of the one a game ships. The engine treats that runtime the same as any
other and needs nothing set here. The setting is global, so it is a way to put a
newer runtime into every game at once, where `[Runtime] Mode=Bundled` does one
game at a time.

The runtime it loads lives in
`%ProgramData%\NVIDIA\NGX\models\dlssg\versions\<build>\files`, named
`<architecture>_<application id>.bin`, and `nvngx_config.txt` beside it lists the
version provisioned per application. A run using it has a `provider_found` line
whose path is in that store, and `sl.log` reports the same version as
`ngxFeatureVersion`. With both the override and `[Runtime] Mode=Bundled` set, the
bundled runtime wins: the redirect applies to whichever runtime is asked for.

Verified on an RTX 3080, driver 616.92, in both APIs. In Crimson Desert
(Direct3D 12) with the override on and no redirect, NGX ran 310.9.0 from the
store, 704 kernels were supplied with none refused and none rejected by the
driver, and `presented` was captured at 2, 3, 4 and 6. With the override on and
`Mode=Bundled` the bundled runtime runs instead, as `ngxFeatureVersion` in
`sl.log` confirms. In DOOM The Dark Ages (Vulkan) the override runtime also
reached 6x, `presented: 6`, which the runtime that game ships did not.

Three `provider_found` lines are normal with the override on: the runtime the
game ships, the store's, and the driver's own older copy, which is the one that
reports `multi_frame_gates_not_found` and never generates a frame.

Expect more than one `provider_found` line in such a run. Besides the game's own
runtime and the store's, NGX loads the driver's own copy from the driver store,
which is older and may report `multi_frame_gates_not_found`. Only the runtime
that appears as the `caller` of `kernel_substituted` is the one generating
frames; the others are read and put aside.

Streamline may run a plugin NVIDIA downloaded instead of the one the game
ships. Its path is under `%ProgramData%\NVIDIA\NGX\models`, and `sl.log` names
it as `Found plugin: ...\<architecture>_<application id>.dll`. The engine treats
it as the component it replaces; the `plugin` field of `flip_metering_forced`
says which copy was patched.

**Before posting a log in public**, know what is in it. Our log records where
the game, its NVIDIA components and this DLL are installed, the GPU, the driver
version and the game's own frame-generation settings. A path under your Windows
profile is written as `%USERPROFILE%`, so your account name is not in it, but an
install path elsewhere is recorded as it is. Streamline's `sl.log` is NVIDIA's
and follows no such rule: read it before attaching it.

When reporting, include the game, its store version, the GPU, the driver version,
and both log files. A `provider_untested` line means the game ships a runtime
version nobody has reported on yet; say whether it worked.

## A newer runtime in an older game

`[Runtime] Mode=Bundled` loads a runtime of your choosing in place of the one a
game ships, without changing anything on disk. It is how 310.9.1 was verified
here, in PRAGMATA and in Crimson Desert, and it is worth trying for its own
sake: on 310.9.1 PRAGMATA generates frames at 2x, 3x and 4x and the ghosting it
showed with path tracing and Ray Reconstruction is gone.

## Tested so far

| Game | API | Runtime | GPU | Result |
|---|---|---|---|---|
| PRAGMATA | Direct3D 12 | 310.3.0, 310.9.0, 310.9.1 | RTX 3080 | On the runtime it ships, 310.3.0: 2x and 4x work, smooth. Verified at 310.9.1 through `[Runtime] Mode=Bundled`: 2x, 3x and 4x, and the ghosting this game showed with path tracing and Ray Reconstruction is gone on that runtime. Verified at 310.9.0 through the DLSS override, with path tracing and Ray Reconstruction on and with ReShade, renodx and ReFramework all loaded beside it: `presented` 3 and 4, 64 kernels supplied, none refused |
| DOOM The Dark Ages | Vulkan | 310.6.0, 310.9.0 | RTX 3080 | On the runtime it ships, 310.6.0: 2x works, 90 to 130 fps, and 3x and 4x work, confirmed at `presented` 2, 3 and 4. It also offers 6x, which was accepted but never read back. On 310.9.0, loaded by the DLSS override, `presented` 3 and 6 were both captured, the last from `numFramesToGenerate=5`, with 192 kernels supplied and none refused |
| Far Far West | Direct3D 12 | 310.6.0 | RTX 3080 | 2x works; its menu only switches generation on and off, so 4x needs `ForceMultiplier=4`, confirmed at `presented: 4` |
| Halo Campaign Evolved | Direct3D 12 | 310.2.1 | RTX 3080 | 2x works; its menu only switches generation on and off, so 4x needs `ForceMultiplier=4`, confirmed at `presented: 4`. Streamline runs plugins NGX downloaded, not the ones the game ships |
| Jurassic World Evolution 3 | Direct3D 12 | 310.3.0 | RTX 3080 | Its menu offers the multipliers; 2x, 3x and 4x confirmed at `presented` 2, 3 and 4 |
| Indiana Jones and the Great Circle | Vulkan | 310.2.1 | RTX 3080 | Its menu offers the multipliers; 2x, 3x and 4x work with path tracing on, `presented: 3` captured and every other request accepted. Streamline runs a plugin NGX downloaded. The game asks for `numFramesToGenerate` 0 when switching modes, which Streamline rejects on its own; that is the game's call, passed through untouched |
| Frostpunk 2 | Direct3D 12 | 310.5.2 | RTX 3080 | Its menu offers the multipliers; 2x, 3x and 4x confirmed at `presented` 2, 3 and 4. A cutscene played at 3x showed artefacts, with `sl.log` reporting 34 `Frame rate over 100.00ms, reseting frame timer` warnings in that minute: the game's own frames were arriving more than 100 ms apart, which is what generation had to interpolate across |
| Crimson Desert | Direct3D 12 | 310.9.1 | RTX 3080 | 310.9.1 loaded through `[Runtime] Mode=Bundled`. Every multiplier its menu offers works, with Ray Reconstruction on: `presented` 2, 3, 4, 5 and 6, the last from `numFramesToGenerate=5`. Streamline 2.11.1 and the runtime both allow 5 generated frames, and nothing in the chain caps it below that, which no other game tested here reaches |
| Corsair Cove | Direct3D 12 | 310.5.2 | RTX 3080 | Its menu offers the multipliers; 2x and 3x confirmed at `presented` 2 and 3. 4x was accepted but the run ended before a state read showed it. Streamline 2.10.3, running plugins NGX downloaded |
