# FLOPPY.PRG

> [!IMPORTANT]
> SideTNFS-Floppy-browser is part of the [SideTNFS](https://github.com/RetroLoft/SideTNFS-Firmware) project.

FLOPPY.PRG is the floppy-image browser and launcher for
[SideTNFS](https://github.com/RetroLoft/SideTNFS-Firmware) on the Atari ST family.

It lets you browse `.ST` floppy images stored on a microSD card or a TNFS
server, build a temporary multi-disk carousel, and start the selected floppy
directly from GEM.

Long filenames are supported while browsing, so disk images do not need to
be renamed to Atari GEMDOS 8.3 filenames.

## Features

- Browse `.ST` floppy images on microSD or TNFS network servers
- Full long-filename browsing
- Up to 8 configurable TNFS or SD sources
- Search within the current directory
- Up to 60 Favorite slots for your most favorite floppy images
- Temporary Carousel for multi-disk games
- Carousel entries may come from different SD or TNFS sources
- Switch to the next floppy with the SideTNFS SELECT button
- Start a single floppy immediately without manually building a Carousel
- Emulates drive A: or B:
- Optional simultaneous use of SideTNFS SD/TNFS GEMDOS drives
- Keyboard and mouse operation

## Browser Sources

FLOPPY.PRG can remember up to 8 places from which you commonly browse floppy
images.

A TNFS source contains a server, port and starting directory, for example:

```text
Retroloft
retroloft.net
16384
/FLOPPIES/ST
```

An SD source similarly remembers the directory where browsing should begin.

Sources are only shortcuts for the Browser. Favorites and Carousel entries
remember the actual location of their images independently.

## File Browser

The Browser shows the floppy images in the selected SD or TNFS directory.

You can move through directories and pages, add images to Favorites, add
images to the Carousel, or start an image immediately.

Double-clicking an image, or pressing Enter on it, adds it to the Carousel.

### Search

Directories containing hundreds of floppy images can be searched without
paging through the complete list.

Search works within the current directory only and performs a
case-insensitive substring match.

For example:

```text
crys
```

may find:

```text
Crystal Castles.st
Crystal Caverns.st
My Crystal Demo.st
```

Search does not recurse into subdirectories.

If no matching images are found, the current Browser listing remains
unchanged.

## Favorites

Favorites are a persistent collection of floppy images you use frequently.

Up to 60 Favorites can be stored.

Favorites are not tied to one Browser source. A single Favorites list can
therefore contain images from:

- the SD card
- one TNFS server
- another TNFS server

For example:

```text
SD      Crystal Castles
TNFS    Nebulus
TNFS    Monkey Island Disk 1
SD      Dungeon Master
```

If the original server, SD card or image is no longer available,
FLOPPY.PRG reports an error when you try to use that Favorite.

The Favorite itself is left unchanged.

## Carousel

The Carousel is a temporary list of floppy images for the game or program
you are about to run.

Unlike Favorites, the Carousel is never saved. Every new run of FLOPPY.PRG
starts with an empty Carousel.

A Carousel can contain up to 8 images.

For example:

```text
1  Monkey Island Disk 1
2  Monkey Island Disk 2
3  Monkey Island Disk 3
4  Monkey Island Disk 4
```

The images do not need to come from the same source. SD and TNFS images, and
even images from different TNFS servers, may be mixed in one Carousel.

The first image is loaded when floppy emulation starts.

While the software is running, pressing SELECT on the SideTNFS cartridge
switches to the next image in the Carousel.

If a later image is unavailable, the currently active floppy remains
inserted.

## Starting a floppy

There are two normal ways to start floppy emulation.

### Start Now

For a single-disk game, simply select the image and use **Start Now**.

FLOPPY.PRG creates a temporary one-image Carousel and starts it immediately.

### Carousel

For a multi-disk game, add the required images to the Carousel, arrange
them in the desired order and start the Carousel.

Typical example:

```text
Monkey Island Disk 1
Monkey Island Disk 2
Monkey Island Disk 3
Monkey Island Disk 4
```

During the game, use SELECT to move through the disks.

## Floppy Only / With SD/TNFS Drives

Before starting the Carousel you can choose whether floppy emulation should
run by itself or together with the normal SideTNFS GEMDOS drives.

**Floppy Only** starts only the virtual floppy environment.

**With SD/TNFS drives** keeps the configured SideTNFS GEMDOS drives available
as well.

The same dialog also lets you choose which drive letter the virtual floppy
is installed as: **A:** (the default) or **B:**, never both at once. B:
must be picked explicitly every time you start the Carousel -- it is never
remembered between runs. **Start Now** always uses drive A:; only the full
Start Floppy Carousel dialog offers B:.

## Keyboard use

FLOPPY.PRG can be operated entirely from the keyboard.

Common controls include:

- Arrow keys to move through lists
- Enter to select or add an image
- Left/Right to change Browser pages
- Backspace to move up one directory
- Esc to cancel or deselect
- Tab to switch between Favorites and Browser
- F1-F4 to select Favorites pages
- S to search or open the source selector where applicable

Mouse operation is supported as well.

## Requirements

FLOPPY.PRG requires a compatible SideTNFS cartridge and firmware.

See the main SideTNFS repository for firmware, hardware information and the
other SideTNFS software:

[RetroLoft/SideTNFS-Firmware](https://github.com/RetroLoft/SideTNFS-Firmware)

## Building from source

FLOPPY.PRG is built with the Atari MiNT cross compiler:

```text
m68k-atari-mint-gcc
```

Build with:

```bash
make
```

The resulting program is:

```text
FLOPPY.PRG
```

To clean the build:

```bash
make clean
```

## Running

Copy `FLOPPY.PRG` to your Atari ST and run it from the GEM Desktop while the
SideTNFS cartridge is installed.

## Known limitations

- Only `.ST` disk images are supported. Other floppy image formats (`.STX`,
  `.MSA`, `.DIM`, ...) are not read.
- Floppy emulation works through the GEMDOS file system vectors. Some games
  and other software bypass GEMDOS entirely -- custom loaders, copy
  protection, or direct floppy controller access -- and will not work
  correctly, or at all, under this kind of emulation.
- Medium or high resolution is required. On a low-resolution screen,
  FLOPPY.PRG shows an explanatory message and exits.
- Only one virtual floppy drive is active at a time: A: or B:, never both
  simultaneously.
- TNFS sources are reached over UDP only; TCP is not supported.