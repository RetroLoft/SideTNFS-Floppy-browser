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

## Keyboard shortcuts

Every screen is fully usable with the keyboard alone -- the mouse is never
required. Up/Down always move a highlighted row; Enter/Space confirm it;
Esc always cancels/backs out.

### Favorites (the main screen)

| Key | Action |
| --- | --- |
| Up / Down | Move the row selection (or, while placing/moving a favorite, the target row) |
| Enter | Add the selected favorite to the Carousel's first available slot; while placing/moving, confirm onto the highlighted row |
| Space | While placing/moving, confirm onto the highlighted row |
| Delete | Erase the selected favorite |
| Ctrl+Delete | Erase all 60 favorites (asks for confirmation) |
| M | Move the selected favorite |
| Esc | Deselect; while placing/moving, cancel it |
| Tab | Open the file Browser (also cancels a pending place/move) |
| S | Open the Source selector (no dedicated button here any more -- see below) |
| F1 / F2 / F3 / F4 | Switch to page 01-15 / 16-30 / 31-45 / 46-60 |
| Undo | Quit |

Bottom button row, left to right: **[Browser] [Start] [Move] [Erase]**,
with **[Quit]** at the far right. There is no **[Source]** button here any
more -- the file Browser's own **[Change]** (next to the current source's
name, see below) does the same job, and the S key still opens the same
selector directly from here.

The **[Start]** button (also present in the Browser, see below) opens the
Start Floppy Carousel dialog, which uploads and boots whatever is
currently in the Carousel (up to 8 slots) -- it does not depend on a
Favorites row being selected.

Only the first Carousel slot is actually checked before Reboot -- Reboot
itself validates and mounts it, so a missing/broken file there shows an
alert and keeps you in FLOPPY.PRG instead of resetting into a broken
state. Slots 2-8 are uploaded as-is with no upfront check; if one of
them turns out to be missing/broken, cycling to it with SELECT on the
cartridge just skips it and keeps the current floppy active, with no
error shown. This asymmetry is intentional, not a bug: checking every
slot upfront would mean a real network round trip per slot (possibly to
several different TNFS servers) before Reboot even runs.

The Start Floppy Carousel dialog itself has three buttons:
**[Reboot] [Empty] [Cancel]**. [Empty] clears every Carousel slot and
closes the dialog without rebooting (same as Cancel otherwise) -- no
confirmation prompt, since the Carousel is only ever a volatile, in-RAM
queue, never saved to disk.

### File Browser

| Key | Action |
| --- | --- |
| Up / Down | Move the row selection |
| Enter | Add the selected file to the Carousel's first available slot (same as a double-click); with nothing selected, same as [Add Fav] |
| [Add Fav] (button only) | Confirm the selected file back to Favorites (place it into a favorite slot) |
| S / [Search] | Search the current directory (see below) |
| Left / Right arrow | Previous / next page |
| Backspace / [Dir Up] | Go up one directory |
| Tab / Undo | Back to Favorites |
| Esc | Deselect the current row |

The current directory line also shows the active source
(`Source: <nickname>   Type: <TNFS/SD>`) with a **[Change]** button next
to it -- opens the same server/source selector Favorites' own [Source]
button used to (that button is gone now, see above), and reopens the
Browser against whichever source ends up active. **[Dir Up]** sits at the
left of this same line. The bottom button row, left to right, is
**[Favorites] [Start] [Add Fav] [Search]**, with **[Prev] [Next]** at the
far right. [Start] here is the same Start Floppy Carousel dialog as
Favorites' own [Start] -- the Carousel is one global queue shared by both
screens.

A double-click on a file also adds it straight to the Carousel, same as
Enter -- this is a shortcut to get a game into the boot rotation directly,
separate from adding it to the Favorites catalog via [Add].

Both the Favorites and File Browser "Added to the Carousel" confirmation
carries a second **[Carousel]** button alongside [OK] -- [OK] stays the
default (Enter/click behaves exactly as before), while [Carousel] jumps
straight into the Start Floppy Carousel dialog instead of just dismissing
the alert.

#### Search

[Search] opens a small dialog asking for a search text, then filters the
**current directory only** to filenames containing that text (case-insensitive,
plain substring match -- no wildcards). Matches replace the normal listing;
the title shows the directory plus the active search text (e.g.
`/floppies/c/   Search: "crys"`), and Prev/Next page through the filtered
results exactly like a normal directory listing. Directories are never
included in search results, and the search only covers the directory you
were in when you opened Search -- it does not recurse into subdirectories.

If nothing matches, an alert says so and the Browser is left exactly as it
was -- it does not switch into search-result mode. Changing directory,
changing source, or leaving/reopening the Browser all clear an active
search automatically (it is never saved anywhere). To go back to the
unfiltered listing without changing directory, open [Search] again and
press [Cancel].

| Key | Action |
| --- | --- |
| (type) | Enter the search text |
| Enter / [Search] | Run the search |
| Esc / [Cancel] | Close without searching; while search results are showing, also clears them |

### Source selector

| Key | Action |
| --- | --- |
| Up / Down | Move the row highlight |
| Enter | Activate the highlighted source and close |
| E | Edit sources... |
| Esc | Cancel |

### Edit Sources overview

| Key | Action |
| --- | --- |
| Up / Down | Move the row highlight |
| Space | Edit/Add the highlighted slot |
| 1-8 | Edit/Add that slot directly, regardless of the current highlight |
| Enter | Same as [OK] |
| S | Save (pushes all 8 profiles to the cartridge's flash) |
| Esc | Cancel |

### Floppy Source editor (Nickname/Host/Port/Start dir/SD path)

| Key | Action |
| --- | --- |
| Tab / Shift+Tab, arrow keys | Move between fields (standard text-field navigation) |
| F1 | Set Source to TNFS |
| F2 | Set Source to SD card |
| Ctrl+A | Toggle Active/Inactive |
| Ctrl+R | Remove this source (asks for confirmation) |
| Esc | Cancel |
| Return | Same as [OK] (when not otherwise consumed by the focused field) |

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
