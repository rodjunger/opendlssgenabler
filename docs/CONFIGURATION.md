# Configuring opendlssg-fg

`opendlssg.ini` sits beside the proxy DLL, next to the game's executable. It is
read once at startup, so restart the game after editing it.

## Start here

**Ship the file as it comes and change nothing.** Every default is the value
that should be running; the rest of the file exists to isolate a fault. Of the
nine games in [TESTING.md](TESTING.md), seven needed no edit at all, and the
other two needed one setting, `ForceMultiplier`, for the reason given below.

There are three reasons to open it at all:

| You want to | Change |
|---|---|
| Use 3x or 4x in a game whose menu only switches frame generation on and off | `ForceMultiplier` |
| Report a problem | `Level=3` and `StreamlineDiagnostics=1` |
| Try a newer NVIDIA runtime than the game ships | `[Runtime] Mode=Bundled` |

Everything else is for narrowing down a failure, and each such setting says so
below.

## File syntax

- Section and key names ignore case. `[Kernels]` and `[kernels]` are the same.
- A switch accepts `1`, `true`, `on` or `yes`, and `0`, `false`, `off` or `no`.
- `;` or `#` starts a comment when it follows a space, so a value containing one,
  such as a path, is kept whole. A line beginning with either is a comment.
- A value outside its range falls back to the default and is reported in the log
  as `config_value_rejected`, naming the key and why. Nothing is silently
  ignored.
- Deleting a key, or the whole file, uses the defaults. The engine runs without
  an `opendlssg.ini`; `attach` records `config_found: false`.

## What each setting does

### `[General]`

**`Enabled`** (default `1`)
`0` forwards the system DLL and does nothing else, leaving the game exactly as it
would be without this file. Use it to confirm a problem is this engine's before
uninstalling.

### `[FrameGeneration]`

**`MultiFrame`** (default `1`)
Lets NVIDIA's runtime generate more than one frame, so a game that supports
multi-frame generation offers 3x and above. The runtime otherwise reserves that
for RTX 50. `0` keeps every game at 2x, which is worth trying if a game is
unstable above 2x.

**`ForceMultiplier`** (default `0`, otherwise `2` to `6`)
`0` follows the multiplier the game asks for. Any other value forces that
multiplier instead.

This is the one setting a working install may still need. The multiplier is the
game's choice: it calls `slDLSSGSetOptions(numFramesToGenerate)`, and a menu that
only switches frame generation on and off always asks for one generated frame,
whatever the hardware allows. Far Far West and Halo Campaign Evolved are both
like this, and both reach 4x with `ForceMultiplier=4`.

It never switches frame generation on. With the game's own setting off, nothing
is forced. If the runtime refuses the count, the game's own request is sent
again so it keeps the frame generation it asked for, and the log records
`dlssg_force_rejected`. A game built against a newer Streamline than this
release knows is not forced at all, and the log records `dlssg_force_skipped`.

### `[Compatibility]`

These settings change NVIDIA's own components, in memory. The defaults are
correct on every GPU and game tested. Each is separately switchable because each
could independently take the device down, which is what makes them useful when
something goes wrong.

**`SpoofArchToGame`** (default `1`)
Reports an Ada architecture to the three components that gate frame generation.
`0` keeps the real architecture and leaves the hooks installed, which confirms
the engine loads and binds without opening the gate. Frame generation will not
be offered.

**`SpoofCallers`** (default `sl.common.dll,_nvngx.dll,nvngx_dlssg.dll`)
Which modules are told the GPU is Ada, comma separated. Three need it, for three
different checks:

| Module | Why |
|---|---|
| `sl.common.dll` | Computes the adapter mask that decides whether the frame-generation plugin loads at all |
| `_nvngx.dll` | Answers the availability query the plugin makes at startup |
| `nvngx_dlssg.dll` | The frame-generation runtime, which does not create its kernels otherwise |

Everything else, the game included, sees the real hardware. A module is matched
by the component it is, not the file name it carries, so a copy NGX downloaded
under a name like `160_E658700.bin` is still recognised as the runtime.

**Keep this list short.** In PRAGMATA with path tracing on, telling every caller
Ada removed the device before the main menu, most likely through the upscaling
runtimes selecting Ada-only code paths. The value `none` tells every caller and
exists to reproduce that, not to run with.

**`PatchFlipMetering`** (default `1`)
Steers the frame-generation plugin onto the software frame pacing it already
carries. Ampere has no hardware flip metering, and without this a Direct3D 12
game generates frames that are never shown.

**`FlipMeteringValue`** (default `-1`, otherwise `0` or `1`)
The value that means "off" for the plugin's metering flag. `-1` reads it from the
plugin's own code, which has been correct for every build checked. Set it only if
a future plugin is read wrongly, which `flip_metering_forced` in the log would
show.

**`PatchFrameClamp`** (default `0`)
Lifts the Streamline plugin's own clamp on the generated-frame count. The plugin
a game ships already allows what that game supports, so this only matters for
testing beyond it.

**`StubScgPriority`** (default `1`)
Answers `NvAPI_D3D12_SetRawScgPriority` with success instead of running it. It is
an Ada-only call that removes the device on older hardware.

### `[Kernels]`

**`Retarget`** (default `1`)
Supplies kernels this GPU can run: NVIDIA's own Ampere build where the runtime
carries one, otherwise its PTX retargeted to this GPU. Without it frame
generation is offered and then has nothing to execute. `0` is a diagnostic, and
the log will fill with refusals.

**`TargetSM`** (default `0`, otherwise `50` to `200`)
The architecture to build kernels for, as an SM number such as `86`. `0` asks the
CUDA driver, which is right unless the driver cannot be reached;
`kernel_target_resolved` names the source it used.

**`VulkanHooks`** (default `1`)
Covers Vulkan games, which hand their kernels to the driver through
`VK_NVX_binary_import` rather than through NVAPI. It costs nothing in a
Direct3D 12 game, where the Vulkan loader is never used.

### `[Runtime]`

**`Mode`** (default `Off`) and **`RuntimeFile`** (default `opendlssg_nvngx_dlssg.dll`)
Experimental. `Off` uses the frame-generation runtime the game ships. `Bundled`
loads `RuntimeFile` from beside this file in its place, which is how a newer
NVIDIA runtime is tried in an older game without changing anything the game
installed. Copy the runtime next to `opendlssg.ini` under that name.

`Bundled` also takes precedence over the runtime the driver's DLSS override
would load. See [TESTING.md](TESTING.md#the-dlss-override) for how the two
interact, and for the global alternative, which needs no setting here.

### `[Logging]`

**`Level`** (default `1`, range `0` to `3`)

| Level | Records |
|---|---|
| `0` | Nothing |
| `1` | Errors and warnings, plus the lines that identify the run |
| `2` | One line per decision |
| `3` | Every call, every kernel, every frame |

`1` is what a bug report needs: it still names the GPU, the driver, the game, the
runtimes and the kernel totals, and it shows any step that failed. Use `3` only
to investigate a specific problem. In Indiana Jones and the Great Circle it
produces a quarter of a million lines.

**`Directory`** (default `opendlssg\logs`)
Relative to this file, or absolute. Each process writes its own
`loader_<pid>.jsonl` and the newest ten are kept. `sl.log` is Streamline's and is
not pruned by this engine, but Streamline truncates it on every launch, so copy
it aside before relaunching if it matters.

### `[Debug]`

**`StreamlineDiagnostics`** (default `0`)
Writes Streamline's own log as `sl.log` beside ours. Streamline states there, and
nowhere else, why it accepts or refuses frame generation, so turn this on for any
report about the feature being missing.

**`DumpKernels`** (default `0`)
Writes the first few kernel images the runtime creates, before and after
substitution, under the log directory. For investigating a kernel the driver
refuses, and nothing else.

## Changing a setting to find a fault

When frame generation is missing or wrong, the log usually names the step that
failed and no setting needs changing. Read [TESTING.md](TESTING.md) first. If it
does not, these turn off one piece at a time, which tells you which piece is
responsible:

| Symptom | Try | What it tells you |
|---|---|---|
| The game crashes or the screen goes black at startup | `Enabled=0` | Whether this engine is involved at all |
| The device is removed before the menu | `SpoofCallers` shortened, or `SpoofArchToGame=0` | Whether a spoofed caller is choosing code the GPU cannot run |
| Frame generation is offered but nothing is generated | `Level=3`, then read `kernel_substituted` and `kernel_refused` | Which kernels were supplied and which were not |
| Unstable above 2x | `MultiFrame=0` | Whether the multi-frame path is the cause |
| A Direct3D 12 game generates frames that never appear | `PatchFlipMetering` is already on; check `flip_metering_forced` in the log | Whether the pacing patch found its site |

Change one setting at a time, and put it back afterwards. A setting left off is a
capability given up.
