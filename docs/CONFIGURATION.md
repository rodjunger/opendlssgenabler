# Configuring opendlssg-fg

`opendlssg.ini` sits beside the proxy DLL, next to the game's executable. It is
read once at startup; restart the game after editing it.

## Start here

**Use the file as shipped.** The defaults are the intended settings; the other
options exist to isolate faults. Of the games in [TESTING.md](TESTING.md), only
two needed a change, both `ForceMultiplier`.

Reasons to edit it:

| You want to | Change |
|---|---|
| Use 3x or 4x in a game whose menu only has on and off | `ForceMultiplier` |
| Report a problem | `Level=3` and `StreamlineDiagnostics=1` |
| Try a newer NVIDIA runtime than the game ships | `[Runtime] Mode=Bundled` |

## File syntax

- Section and key names are case-insensitive.
- Switches accept `1`, `true`, `on`, `yes` and `0`, `false`, `off`, `no`.
- A line starting with `;` or `#` is a comment. After a value, `;` or `#` starts
  a comment only when preceded by a space, so paths containing them are kept.
- An out-of-range value falls back to the default and is logged as
  `config_value_rejected`, with the key and the reason.
- Missing keys use their defaults. Without the file, everything does, and
  `attach` logs `config_found: false`.

## Settings

### `[General]`

**`Enabled`** (default `1`)
`0` only forwards the system DLL. Use it to check whether a problem is caused by
this project.

### `[FrameGeneration]`

**`MultiFrame`** (default `1`)
Lets the runtime generate more than one frame, which it otherwise reserves for
RTX 50, so games that support it offer 3x and above. `0` limits every game to
2x; try it if a game is unstable above 2x.

**`ForceMultiplier`** (default `0`, otherwise `2` to `6`)
`0` uses the multiplier the game asks for. Any other value replaces it.

The game sets the multiplier through `slDLSSGSetOptions(numFramesToGenerate)`.
A menu with only on and off always asks for 2x. Far Far West and Halo Campaign
Evolved are like this, and both reach 4x with `ForceMultiplier=4`.

- It never turns frame generation on; with the game's setting off, nothing is
  forced.
- If the runtime refuses the count, the game's own request is sent instead and
  `dlssg_force_rejected` is logged.
- A game built against a newer Streamline than this release knows is not forced,
  and `dlssg_force_skipped` is logged.

### `[Compatibility]`

These settings patch NVIDIA's components in memory. The defaults are correct on
every tested GPU and game. Each can be switched off separately to find which one
causes a crash.

**`SpoofArchToGame`** (default `1`)
Reports an Ada GPU to the components that gate frame generation. `0` keeps the
hooks but reports the real architecture: useful to check that the DLL loads,
but frame generation will not be offered.

**`SpoofCallers`** (default `sl.common.dll,_nvngx.dll,nvngx_dlssg.dll`)
Comma-separated modules that are told the GPU is Ada:

| Module | Why |
|---|---|
| `sl.common.dll` | Computes the adapter mask that decides whether the frame-generation plugin loads |
| `_nvngx.dll` | Answers the plugin's availability query |
| `nvngx_dlssg.dll` | The runtime; it creates no kernels otherwise |

Every other module, including the game, sees the real GPU. Modules are matched
by what they are, not their file name, so a copy NGX downloaded as
`160_E658700.bin` is still recognised as the runtime.

**Keep this list short.** Telling every module Ada removed the device in
PRAGMATA with path tracing on. `none` tells every module; it exists to
reproduce that, not for normal use.

**`PatchFlipMetering`** (default `1`)
Fixes frame pacing on Ampere, in two places:

- Makes the plugin use its software frame pacing. Ampere has no hardware flip
  metering, so without this a Direct3D 12 game generates frames that are never
  shown.
- In a Vulkan game that turns Reflex on but never calls its sleep, such as No
  Man's Sky, passes Reflex low-latency mode to the driver as off while frame
  generation is on. Otherwise the driver paces every generated frame as a whole
  one, and frame generation halves the frame rate.

**`FlipMeteringValue`** (default `-1`, otherwise `0` or `1`)
The plugin's "off" value for its metering flag. `-1` reads it from the plugin's
code, which has worked on every build checked. Override it only if
`flip_metering_forced` shows a wrong value.

**`PatchFrameClamp`** (default `0`)
Removes the plugin's limit on generated frames. The plugin a game ships already
allows what the game supports, so this is only for testing.

**`StubScgPriority`** (default `1`)
Returns success from `NvAPI_D3D12_SetRawScgPriority` without calling it. The call
is Ada-only and removes the device on older GPUs.

### `[Kernels]`

**`Retarget`** (default `1`)
Supplies kernels this GPU can run: NVIDIA's Ampere build where the runtime has
one, otherwise its PTX recompiled for this GPU. With `0`, frame generation is
offered but has no kernels to run; images this GPU cannot run are still refused
rather than passed to the driver. Diagnostic only.

**`TargetSM`** (default `0`, otherwise `50` to `200`)
The architecture to build kernels for, such as `86`. `0` asks the CUDA driver.
`kernel_target_resolved` logs which source was used.

**`VulkanHooks`** (default `1`)
Handles Vulkan games, which load kernels through `VK_NVX_binary_import` instead
of NVAPI. It has no effect in Direct3D 12 games. It also carries the Reflex fix of
`PatchFlipMetering`.

### `[Runtime]`

**`Mode`** (default `Off`) and **`RuntimeFile`** (default `opendlssg_nvngx_dlssg.dll`)
Experimental. `Off` uses the game's runtime. `Bundled` loads `RuntimeFile` from
beside this file instead, so you can try a newer runtime without changing the
game's files. Copy the runtime next to `opendlssg.ini` under that name.

`Bundled` also overrides the driver's DLSS override. See
[TESTING.md](TESTING.md#the-dlss-override).

### `[Logging]`

**`Level`** (default `1`, range `0` to `3`)

| Level | Records |
|---|---|
| `0` | Nothing |
| `1` | Errors, warnings, and the lines that identify the run |
| `2` | Also one line per decision |
| `3` | Also every call, kernel and frame |

Level `1` is enough for a bug report: it names the GPU, driver, game, runtimes
and kernel totals, and shows every failed step. Level `3` is for investigation;
Indiana Jones and the Great Circle produces about 250,000 lines at it.

**`Directory`** (default `opendlssg\logs`)
Relative to this file, or absolute. Each process writes `loader_<pid>.jsonl`;
the newest ten are kept. `sl.log` is not pruned, but Streamline overwrites it on
every launch, so copy it before relaunching if you need it.

### `[Debug]`

**`StreamlineDiagnostics`** (default `0`)
Writes Streamline's log as `sl.log` beside ours. It is the only place Streamline
says why it accepts or refuses frame generation, so enable it when reporting a
missing feature.

**`DumpKernels`** (default `0`)
Writes the first few kernel images, before and after substitution, to the log
directory. Only for investigating a kernel the driver refuses.

## Finding a fault

Check the log first (see [TESTING.md](TESTING.md)); it usually names the failed
step. Otherwise, switch off one piece at a time:

| Symptom | Try | Tells you |
|---|---|---|
| Crash or black screen at startup | `Enabled=0` | Whether this project is involved |
| Device removed before the menu | Shorter `SpoofCallers`, or `SpoofArchToGame=0` | Whether a spoofed module picks code the GPU cannot run |
| Frame generation offered, nothing generated | `Level=3`, read `kernel_substituted` and `kernel_refused` | Which kernels were and were not supplied |
| Unstable above 2x | `MultiFrame=0` | Whether multi-frame is the cause |
| Direct3D 12 frames generated but never shown | Check `flip_metering_forced` | Whether the pacing patch found its site |

Change one setting at a time and restore it afterwards.
