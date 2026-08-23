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

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
