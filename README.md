# FLOPPY.PRG

A GEM application for the Atari ST family that lets a SideTNFS cartridge
browse TNFS network shares for floppy disk images, independently of the
existing SideTNFS GEMDOS drive configuration.

## What this is

[SideTNFS](https://github.com/RetroLoft/SideTNFS-Firmware) is a cartridge
for the Atari ST that emulates a GEMDOS hard drive over WiFi (TNFS) or a
microSD card, using a Raspberry Pi Pico. It already ships a configuration
tool, `SIDETNFS.PRG` ([SideTNFS-Config](https://github.com/RetroLoft/SideTNFS-Config)),
for setting up GEMDOS drive letters, WiFi, and the clock.

`FLOPPY.PRG` is a **separate** GEM application, not another SideTNFS
configuration screen. It reuses the same SideTNFS cartridge connection, but
manages its own independent list of up to 8 TNFS *server profiles* -- plain
network sources to browse for `.ST` floppy images, with no GEMDOS drive
letter attached to any of them. The eventual goal (later project phases,
not part of this repository yet) is to let the user browse a profile's
files with full original long filenames -- bypassing the Atari's GEMDOS 8.3
filename limit entirely for this purpose -- and pick one.

This repository is independent of `SideTNFS-Firmware`/`SideTNFS-Config`/
`SideTNFS-Gemdrive`: it is developed here, builds its own `.PRG`, and talks
to the firmware over the cartridge's existing ROM3 protocol. It does not
depend on those repositories at build time (only on their protocol
conventions, followed by hand).

## Current project scope (Step 1)

This is the first implementation phase. It provides:

- The `FLOPPY.PRG` GEM application skeleton (medium/high resolution only;
  low resolution shows an explanatory alert and exits).
- A Server selector (`[Server]` action) listing up to 8 saved server
  profiles, and an "Edit servers..." dialog for adding/editing/removing
  them (nickname, host, port, mount directory).
- The matching firmware-side changes (on the `SideTNFS-Firmware` repository's
  own `floppyemu` branch, not merged into `main`): an independent flash
  sector for the 8 profiles, and the minimal cartridge protocol commands
  needed to read/write/save them.

**Not yet implemented** (later phases):

- The long-filename directory browser and file selector.
- Confirming a selected file back to the Pico.
- Any floppy image mounting, validation, or emulation. `FLOPPY.PRG` does
  not (and for a long time will not) emulate a floppy drive.

See `RESEARCH-STEP0.md` in this repository for the architecture research
and design rationale this project is built on.

## Requirements

Building `FLOPPY.PRG` needs a cross-compiler that runs *on your Linux/PC
and outputs Atari ST 68000/TOS binaries* -- see
[SideTNFS-Config's README](https://github.com/RetroLoft/SideTNFS-Config)
for the exact toolchain (`m68k-atari-mint-gcc`, part of the MiNT
cross-tools) and installation options; the same toolchain builds this
project.

## Build

```
make
```

Output: `FLOPPY.PRG`.

## Clean

```
make clean
```

## Run

Transfer `FLOPPY.PRG` to an Atari ST (or emulator such as Hatari) with a
SideTNFS cartridge/Pico present, and run it from the GEM desktop.
