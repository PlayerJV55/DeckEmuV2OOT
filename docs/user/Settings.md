# User Handbook - Settings

As the emulator continues to grow, so does the number of settings that come and go.

Most of the development adds new settings that enhance performance/compatibility, only to be removed later in newer versions due to newfound discoveries or because they were "a hacky workaround".

As such, this guide will NOT mention those kind of settings, we'd rather mention settings which have a long shelf time (i.e won't get removed in future releases) and are likely to be unchanged.

Some of the options are self explanatory, and they do exactly what they say they do (i.e "Pause when not in focus"); such options will be also skipped due to triviality.

## Foreword

Before touching the settings, see the game boots with stock options. We try our best to ensure users can boot any game using the default settings. If they don't work, then you may try fiddling with options - but please, first use stock options.

## General

- `General/Force X11 as graphics backend`: Wayland on *NIX has prominent issues that are unlikely to be resolved; the kind that are "not our fault, it's Wayland issue", this "temporary" hack forces X11 as the backend, regardless of the desktop manager's default.
- `General/Enable Feral GameMode`: This only does anything when you have Feral Interactive's Gamemode library installed somewhere, if you do, this will help boost FPS by telling the OS to explicitly prioritize *this* application for "gaming" - only for *NIX systems.
- `Interface/Language`: Changes language *of the interface* NOT the emulated program!
- `Interface/Show Current Game in your Discord Status`: Shows the game you are playing in your Discord status (Discord Rich Presence).
- `Interface/Box Art URL`: Where Nemyu looks for box art first, as `<URL><game-name>.png`; by default this is the [Nemyu box art repository](https://gitlab.com/nemyu-emulator/boxart). Eden's box art repository, which covers Switch 1 games, is tried after it. Box art is downloaded once into the `boxart` folder of the Nemyu data folder and is shown in the game list for games whose dumps have no icon (such as loose dumps), on the loading screen and in your Discord status.

## System

- `System/Fixed RNG seed`: Set to 0 (and uncheck) to disable ASLR system-wide (this makes mods like CTGP to stop working); by default it enables ASLR to replicate console behavior.

## Emulation

- `Fastmem`, aka. `Emulation/Enable host MMU emulation (fastmem)`: Enables "fastmem"; a detailed description of fastmem can be found [here](../dynarmic/Design.md#fast-memory-fastmem).
- `Emulation/Unfuse FMA`: Enables deliberate inaccurate FMA behavior which may affect how FMA returns any given operation - this may introduce tiny floating point errors which can cascade in sensitive code (i.e FFmpeg).
- `Emulation/Faster FRSQRTE and FRECPE`: Introduces accuracy errors on square root and reciprocals in exchange for less checks - this introduces inaccuracies with some cases but it's mostly safe.
- `Emulation/Faster ASIMD instructions`: Skips rounding mode checks for ARM ASIMD instructions - this means some code depending on these rounding modes may misbehave.
- `Emulation/Disable address space checks`: Before each memory access, the emulator checks the address is in range, if not it faults; this option makes it so the emulator skips the check entirely (which may be expensive for a myriad of reasons). However at the same time this allows the guest program to "break out" of the emulation context by writing to arbitrary addresses.
- `Emulation/Ignore global monitor`: This relies on a quirk present on x86 to avoid the ARM global monitor emulation, this may increase performance in mutex-heavy contexts (i.e games waiting for next frames or such); but also can cause deadlocks and fun to debug issues.

It is important to note the majority of precision-reducing instructions do not benefit cases where they are not used, which means the performance gains will vary per game.

## Graphics

See also [an extended breakdown of some options](./Graphics.md).

- `Frame Generation/Enable frame generation`: Shows generated frames in between the frames a game renders; the related options are described in [Frame Generation](./Graphics.md#frame-generation).
- `Fixes/Extended dynamic state` and `Fixes/Vertex input dynamic state`: These Vulkan extensions essentially allow you to reuse the same pipeline but just change the state between calls (so called "dynamic state"); the "extended" levels signifies how much state can be placed on this "dynamic" range, for example the amount of depth culling to use can be placed on the dynamic state, avoiding costly reloads and flushes. While this by itself is a fine option, SOME vendors (notably PowerVR and Mali) have problems with anything related to EDS3. EDS3 contains EDS2, and EDS2 contains EDS1. Essentially this means more extended data the driver has to keep track of, at the benefit of avoiding costly flushes.
- `Advanced/Persistent pipeline cache`: This saves compiled shaders onto the disk, independent of any driver's own disk saved shaders (yes, some drivers, notably NVIDIA, save a secondary shader cache onto disk) - disable this only if you're debugging or working on the GPU backend. This option is meant to massively help to reduce shader stutters (after playing for one session that compiles them).
- `Advanced/Vulkan driver pipeline cache`: This is NOT the same as `Persistent pipeline cache`; it's a separate flag that tells the Vulkan backend to create pipeline caches, which are a detail that can be used to massively improve performance and remove pipeline creation overhead. This is a Vulkan feature.

## Network

- `Network/Enable Airplane Mode`: Enable this if a game is crashing before loading AND the logs mention anything related to "web" or "internet" services.

## Controls

See [controllers](./Controllers.md).

- `Hotkeys` (its own entry in the list): Deceptively to remove a hotkey you must right click and a menu will appear to remove that specific hotkey.

## Debug

- `Debug/Enable Auto-Stub`: May help to "fix" some games by just lying and saying that everything they do returns "success" instead of outright crashing for any function/service that is NOT implemented.
- `Debug/Show log in console`: Does as said, note that the program may need to be reopened (Windows) for changes to take effect.
- `Debug/Flush log output`: Classically, every write to the log is "buffered", that is, changes aren't written to the disk UNTIL the program has decided it is time to write, until then it keeps data in a buffer which resides on RAM. If the program crashes, the OS will automatically discard said buffer (any RAM associated with a dead process is automatically discarded/reused for some other purpose); this means critical data may not be logged to the disk on time, which may lead to missing log lines. Use this if you're wanting to remove that factor when debugging, sometimes a hard crash may "eat" some of the log lines IF this option isn't enabled.
- `Debug/Disable Macro HLE`: The emulator has HLE emulation of macro programs for Maxwell, this means that some details are purposefully skipped; this option forces all macro programs to be ran without skipping anything.
