# User Handbook - Third party tools and extras

Nemyu by itself lacks some functionality - or otherwise requires external files (such as packaging) to operate correctly in a given OS.

While most of the links mentioned in this guide are relatively "safe"; we urge users to use their due diligence and appropriately verify the integrity of all files downloaded and ensure they're not compromised.

## Source repository

The main Nemyu repository is always at <https://gitlab.com/nemyu-emulator/nemyu>, and releases are published on its [Releases page](https://gitlab.com/nemyu-emulator/nemyu/-/releases).

Other copies obviously exist on the internet, but we can't guarantee their reliability and/or availability.

## Configuring GameMode

There is a checkbox to enable gamemode automatically. The `libgamemode.so` library must be findable on the standard `LD_LIBRARY_PATH` otherwise it will not properly be enabled. If for whatever reason it doesn't work, see [Arch wiki: GameMode](https://wiki.archlinux.org/title/GameMode) for more info.

You may launch the emulator directly via the wrapper `gamemode <program>`, and things should work out of the box.
