CC         = m68k-atari-mint-gcc

# -m68000          : strict 68000 instruction set (no 68020/030/FPU)
# -O1              : safe optimisation level, avoids 68020 idioms from -O2
# -fno-strict-aliasing : safe ob_spec union access via .index / .tedinfo
# -D__GEMLIB_OLDNAMES  : expose NONE, EXIT, SELECTED, etc. from mt_gem.h
CFLAGS     = -Wall -Wextra -O1 -m68000 -fno-strict-aliasing \
             -I include -D__GEMLIB_OLDNAMES

# -s : strip symbols from the final executable
LDFLAGS    = -s -lgem

TARGET      = FLOPPY.PRG
SRCS        = src/main.c src/profile.c src/floppy_probe.c src/dialog.c
INSTALLDIR  = /mnt/retroloft/retro/Atari.ST/CONFIG

# Step 2 temporary protocol test driver (src/floptest.c) -- plain GEMDOS
# console program, no AES/-lgem needed. NOT part of 'all': it is not the
# final Step 3 browser and must not clutter the shared CONFIG disk on
# every ordinary FLOPPY.PRG build. Build/install explicitly with
# 'make floptest'; delete this target (and src/floptest.c) once Step 3
# wires the real browser into dialog.c -- see the Step 2 report.
TESTTARGET  = FLOPTEST.PRG
TESTSRCS    = src/floptest.c src/floppy_probe.c

# 'all' always installs too, same convention SideTNFS-Config's own
# Makefile uses: every successful build is copied straight to the CONFIG
# share, no separate 'make install' step needed.
all: $(TARGET) install

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) $(LDFLAGS)

install: $(TARGET)
	cp -v $(TARGET) $(INSTALLDIR)/

floptest: $(TESTTARGET) install-floptest

$(TESTTARGET): $(TESTSRCS)
	$(CC) $(CFLAGS) -o $(TESTTARGET) $(TESTSRCS) -s

install-floptest: $(TESTTARGET)
	cp -v $(TESTTARGET) $(INSTALLDIR)/

clean:
	rm -f $(TARGET) $(TESTTARGET)

.PHONY: all install clean floptest install-floptest
