# FLOPPY.PRG — Step 0 Research &amp; Architecture Report

**Status: RESEARCH ONLY. No implementation code exists yet. No source in any referenced
repository was modified while producing this report.**

Convention used throughout: **[FACT]** = directly observed in source, with `file:line`
citations. **[INFERENCE]** = reasonable conclusion drawn from facts, not itself directly
stated in source. **[RECOMMENDATION]** = this report's own proposal for the next phase.
**[OPEN QUESTION]** = could not be resolved from available source; flagged for the user
or for later implementation phases.

---

## 1. Executive Summary

SideTNFS is a three-repository product (plus one legacy predecessor and one unmerged
prototype), all of which were inspected read-only for this report:

| Repo (sibling dir) | Role |
|---|---|
| `SideTNFS-Firmware` | Pico W firmware. `romemul/` is the single running application — one dispatch loop, `init_gemdrvemul()`, handles *everything* (GEMDOS relay, TNFS client, all config protocols). Never returns. |
| `SideTNFS-Config` | Source of `SIDETNFS.PRG`, the existing GEM configuration app. Embedded into firmware flash as a read-only virtual disk. |
| `SideTNFS-Gemdrive` | Source of `GEMDRIVE.BIN`, the boot-time 68000-assembly GEMDOS driver that makes TNFS/SD drives appear as ordinary GEMDOS drives. Embedded into firmware flash as ROM. |
| `sidecart-configurator-atari` | **Legacy, different cartridge generation.** VT52 text-console app, not GEM. Its protocol is not applicable to current firmware, but its UI *pattern* (one generic list-pager reused by three different screens) is a useful precedent. |
| `long-filenames` | **Unmerged prototype.** A ready-to-integrate LFN alias/cache module (`sidetnfs_lfn.c/h`), explicitly designed to sit on top of the exact TNFS directory-read code path this project needs, but not wired into `romemul/` today. |

**The most important findings:**

1. **The original TNFS long filename exists in Pico RAM for exactly one function call, then is thrown away.** It is never persisted anywhere else in the current firmware. There is one, and only one, hook point: `sidetnfs_probe.c:4039-4052` inside `fslisting_parse_batch()` (§5).
2. **A ready-to-integrate LFN module already exists** (`../long-filenames/tests/host_lfn/sidetnfs_lfn.c`) and was explicitly designed to be dropped into `romemul/` unchanged, receiving exactly the raw name this hook point produces (§5).
3. **A clean, obvious command-ID range is free**: `APP_GEMDRVEMUL` subcommands `0x1D`–`0x35` (30 codes), confirmed free in both the firmware and the reference Atari driver as of the current docs (§4).
4. **The current firmware's flash-config architecture already demonstrates, twice, exactly the pattern a new floppy-profile store needs** (own magic/version/CRC32 sector, validate-cascade, wholesale-fallback) — and ~880 KB of completely unused flash sits immediately after the last such sector (§8).
5. **Pico general-purpose RAM is tight (~16 KB free, measured from the actual build)**, but the ROM3 shared-memory window has ~46.6 KB free and already hosts a 16 KB buffer for file I/O — this, not Pico heap RAM, is the natural home for a directory-page buffer (§7).
6. **No paging/list-batching mechanism exists anywhere in the live protocol today.** Legacy `LIST_FLOPPIES`/`QUERY_FLOPPY_DB`/etc. commands are defined but have zero handlers in current firmware — they are dead code from an older architecture (§4, §10).
7. **`SIDETNFS.PRG`'s GEM UI is entirely hand-built C (no `.rsc` file)**, using a small set of genuinely generic helpers plus one clear, never-actually-factored-out "run a modal dialog" pattern repeated ~10 times — the single best refactor-and-reuse opportunity for `FLOPPY.PRG` (§3).

None of this requires changing the flash layout, the GEMDOS-drive model, or any existing
command. The architecture is additive: new flash sector, new command-ID range, new GEM
app sharing `SIDETNFS.PRG`'s plumbing but none of its WiFi/RTC/drive-letter logic.

---

## 2. Relevant Repository / Source Structure

```
../SideTNFS-Firmware/                  Pico W firmware (the only thing that runs on the cartridge)
    romemul/gemdrvemul.c                Single command-dispatch loop (init_gemdrvemul, never returns)
    romemul/sidetnfs_probe.c            Pico-side TNFS wire client (~4400 lines) — NOT the same file
                                         as SideTNFS-Config's sidetnfs_probe.c, see §10 naming collision
    romemul/sidetnfs_config.c/.h        Flash-backed 8-drive GEMDOS drive list (magic "STDF")
    romemul/sidetnfs_system_config.c/.h Flash-backed WiFi+Network+RTC (magic "SYSC")
    romemul/config.c                    Legacy 8KB key/value ConfigEntry store (pre-SideTNFS)
    romemul/filesys.c                   SD/FatFS backend — separate 8.3-mangling code path
    romemul/include/commands.h          All command IDs (APP_* namespaces)
    romemul/include/gemdrvemul.h        Full ROM3 shared-memory offset map
    romemul/memmap_romemul.ld           RAM/flash linker layout
    docs/sidetnfs-config-protocol.md    Authoritative protocol contract doc (Dutch), extremely detailed

../SideTNFS-Config/                    Source of SIDETNFS.PRG (the existing GEM config app)
    src/main.c                          appl_init, low-res guard, dialog_run()
    src/dialog.c                        3920 lines — ALL GEM UI, hand-built OBJECT trees, no .rsc
    src/drive.c / include/drive.h       8-slot GEMDOS drive UI model
    src/netconfig.c, rtcconfig.c        Thin WiFi/RTC UI models
    src/sidetnfs_probe.c                Atari-side ROM3 transport client (protocol layer)

../SideTNFS-Gemdrive/                  Source of GEMDRIVE.BIN (boot-time GEMDOS driver, pure 68k asm)
    src/gemdrive.s                      2274 lines — the driver itself, Fsfirst/Fsnext/Fread/etc.
    src/inc/sidecart_functions.s        send_sync/send_write_sync — low-level ROM3 transport primitives
    src/main.s                          Cartridge ROM header + bootstrap

../sidecart-configurator-atari/        LEGACY predecessor product (different cartridge generation)
    configurator/src/floppyselector.c   SD-only floppy browse/load, no "server" concept
    configurator/src/floppydb.c         Network-backed alphabetic-filter browse (closest old analog)
    configurator/src/romselector.c      Structurally identical selector for ROM images
    configurator/src/helper.c           display_paginated_content() — the ONE shared generic pager

../long-filenames/                     UNMERGED PROTOTYPE, not integrated into any running firmware
    tests/host_lfn/sidetnfs_lfn.c/.h    LFN alias/cache module, designed to drop into romemul/ as-is

../gemdrive-test/                      Atari-side GEMDOS test suite (confirms DTA filename field is 14 bytes)
../demo-gemdrive-atari/                Test harness / Hatari fixture, not further inspected
```

**[OPEN QUESTION]** The task prompt referenced `../SideTNFS-Configuration`; no such directory
exists. `../SideTNFS-Config` (confirmed, from its own README, to be the actual source of
`SIDETNFS.PRG`) was used instead, and is almost certainly what was meant.

---

## 3. `SIDETNFS.PRG` Reuse Analysis

### 3.1 GEM/AES architecture [FACT]

- **No `.rsc` resource file exists anywhere in the repo** (`find -iname *.rsc` empty), confirmed
  by `dialog.c`'s own header comment: *"GEM dialogs built in C, no .RSC file."* Every dialog is a
  hand-written `OBJECT[]` array, sized via an `enum` of field indices.
- **No AES windows, no menu bar.** `grep -n "wind_open\|wind_create\|menu_bar" dialog.c` → zero
  matches. This is a pure modal-dialog app: every screen is a `form_dial()`/`form_do()` cycle.
  The one exception is the top-level status window, which runs its own `evnt_multi()` loop
  (`sw_form_do_ticking()`, `dialog.c:3743-3795`) instead of `form_do()`, purely so it can add a
  1-second clock tick (`MU_TIMER`) — `form_do()` has no timer hook.
- **Startup flow** (`main.c:24-68`): `appl_init()` → low-res guard (§3.3) → `graf_mouse(ARROW, 0)`
  → `drive_config_init_defaults(&cfg)` → `dialog_run(&cfg)` → `appl_exit()`.
- **`dialog_run()`** (`dialog.c:3801-3920`): loads drive/network/RTC config from the firmware
  (behind a "please wait" modal, since each probe can time out over ROM3), shows a status
  overview window, then a flat `switch` on which button closed it dispatches to
  `netconfig_editor_run()` / `drives_window_run()` / save / quit. Sub-dialogs are called
  synchronously and recursively — no separate state-machine table, no async anything.

### 3.2 Dialog construction pattern (representative example) [FACT]

Every dialog follows the same five steps, hand-copied ~10 times rather than factored into a
shared function — quoted from the RTC editor (`dialog.c:2228-2276`), the simplest case:

```c
rc_dialog_init();
rc_load_from_config(rc);
form_center(rc_dlg, &x, &y, &w, &h);
form_dial(FMD_START, x, y, w, h, x, y, w, h);
objc_draw(rc_dlg, RC_ROOT, MAX_DEPTH, x, y, w, h);
done = 0;
while (!done) {
    which = (short)(form_do(rc_dlg, start_obj) & 0x7FFF);
    wait_mouse_release();
    switch (which) { /* per-button handling */ }
}
form_dial(FMD_FINISH, x, y, w, h, x, y, w, h);
```

Object construction itself is resolution-independent by design: every `_init()` reads character-cell
metrics via `graf_handle(&cw, &ch, &bw, &bh)` and lays out geometry in units of `cw`/`ch`, not fixed
pixels (`dialog.c:2110` and 10 other call sites) — so the same code scales to any resolution GEM
reports, with no `Getrez()`-conditional branching anywhere in `dialog.c` itself (see §3.4).

### 3.3 Low-resolution handling [FACT]

Already exactly what the new app needs, quoted in full (`main.c:47-57`):

```c
if (Getrez() == 0) {
    /* Cancel is the default (button 1): rebooting the whole machine
     * is easy to trigger by accident otherwise. */
    if (form_alert(1, "[3][SideTNFS Config|This program requires|Medium or High resolution.]"
                      "[Cancel|Switch & Reboot]") == 2) {
        Setscreen(-1L, -1L, 1);
        Supexec(atari_do_reset);
    }
    appl_exit();
    return 0;
}
```

The surrounding comment explains *why* this shape was chosen: dialogs are 50 columns wide (400px
at the standard 8px font) — a genuine width overflow low-res can't absorb — and runtime
`Setscreen()` without a full reset corrupts AES palette/layout on real hardware, so the offered
fix is Setscreen()-immediately-followed-by-reset, matching how GEM's own resolution changer works
internally.

**[RECOMMENDATION]** Per the task's explicit brief ("FLOPPY.PRG should NOT change the Atari
resolution itself"), copy the `form_alert` + `appl_exit()` branch verbatim but **drop the
Setscreen()+reset offer entirely** — show the explanatory alert and exit, nothing else.

### 3.4 Additional resolution handling beyond `main.c` [FACT]

`grep -n "Getrez" dialog.c` → zero matches. The only resolution-*adjacent* logic is a work-area-height
check, copy-pasted into every dialog's `_init()` (e.g. `dialog.c:2111-2116`):

```c
wind_get(0, WF_WORKXYWH, &sx, &sy, &sw, &sh);
if (sh <= 0 || sh > 800) sh = 200;
rh    = (sh >= 350) ? (int)ch : ((ch > 8) ? ch / 2 : (int)ch);
```

This halves row height on short screens so an 8-row dialog still fits. **[RECOMMENDATION]** worth
factoring into one shared `layout_metrics()` helper in the new app rather than re-copying an 11th
time — a concrete, low-risk cleanup opportunity `SIDETNFS.PRG` itself never took.

### 3.5 The Drives dialog [FACT]

Object-ID scheme (`dialog.c:194-208`): 8 fixed rows (`MAX_DRIVES=8`), each one `G_STRING` text
object + one `G_BUTTON` object, built in a loop. Row text/button label refresh via
`ov_refresh_rows()` — empty slots show `"1  Empty"` + an **Add** button; configured slots show
formatted state/letter/type/nickname + an **Edit** button. **There is no separate
Enable/Disable/Remove button on the row itself** — those actions live *inside* the per-slot
editor reached by clicking Edit, not on the overview screen. Row→slot dispatch is done by integer
arithmetic on the clicked object's ID (`dialog.c:3144-3203`), not a lookup table.

**Keyboard shortcuts: proven absent for this dialog.** `grep -n "MU_KEYBD\|evnt_keybd"
dialog.c` only ever matches inside the status window's own tick loop — never inside
`drives_window_run()` or any editor. All keyboard interaction here is GEM's default `form_do()`
Tab/Return handling; there is no hand-coded hotkey/underlined-letter convention anywhere in this
file.

**[OPEN QUESTION]** (from the UI-research agent) No selectable-list-plus-action-buttons pattern
exists anywhere in this codebase to draw from — only this "8 always-visible rows, each with its
own inline button" pattern. Whether `FLOPPY.PRG`'s Server selector (up to 8 nicknames) should copy
this shape, or adopt a true single-selection list, is a design choice for the next phase, not
something the existing code settles.

### 3.6 No shared "run this dialog" helper exists [FACT]

Despite the identical 5-step pattern in §3.2 repeating in `cl_editor_run`, `sd_editor_run`,
`tnfs_editor_run`, `add_disk_type_run`, `rtc_editor_run`, `netconfig_editor_run`, and
`drives_window_run`, there is **no shared function** that runs a dialog and returns which button
was clicked — it is hand-copied every time. **This is the single strongest concrete reuse/refactor
opportunity for `FLOPPY.PRG`**, which will need several of its own dialogs (server list, server
editor, file browser, confirmation).

### 3.7 Tight-coupling audit — what NOT to copy [FACT]

| Concern | Coupled files/symbols |
|---|---|
| **WiFi/network** | `include/netconfig.h`, `src/netconfig.c`; in `dialog.c`: the entire `NC_*` object enum, `nc_dlg[]`, all `buf_nc_*`/`tmpl_nc_*`/`ti_nc_*` statics, `g_netconfig`, `wire_to_ui_netconfig()`/`ui_to_wire_netconfig()`, `fetch_network_config_from_firmware()`, `validate_netconfig()`, `netconfig_editor_run()` |
| **RTC/NTP** | `include/rtcconfig.h`, `src/rtcconfig.c`; in `dialog.c`: `RC_*` enum, `rc_dlg[]`, `g_rtcconfig`, `wire_to_ui_rtcconfig()`, `fetch_rtc_config_from_firmware()`, `rtc_editor_run()`, plus the clock-display block (`format_utc_offset()`, `rtc_sync_text()`) |
| **GEMDOS drives / drive letters** | `include/drive.h`, `src/drive.c` (entire files); in `dialog.c`: `CL_*`/`SD_*`/`TE_*`/`AD_*`/`OV_*` enums and dialogs, `validate_drive()`, `wire_to_ui_drive()`/`ui_to_wire_drive()`, `fetch_drive_config_from_firmware()`, `save_to_firmware()`, `drives_window_run()` |
| **"Current SideTNFS status" overview** | The entire `SW_*` status-window block — inherently a composite of drive+network+RTC+firmware-version text, not generic by definition |

### 3.8 Concrete reuse plan

**A. REUSE AS-IS**

| Function | Location | Purpose |
|---|---|---|
| `set_obj()` | `dialog.c:488-498` | Generic OBJECT geometry/type/flags setter |
| `wire_tree()` | `dialog.c:501-516` | Generic flat-tree `ob_next`/`ob_head`/`ob_tail`/`LASTOB` linker |
| `init_ti()` | `dialog.c:471-485` | Generic TEDINFO constructor for editable fields |
| `fill_n()`, `set_buf()` | `dialog.c:453-469` | Generic edit-buffer fill/copy helpers |
| `buf_nonempty()`, `buf_copy()` | `dialog.c:519-535` | Generic "read a GEM edit field back out" helpers (strips trailing-space padding) |
| `wait_mouse_release()` | `dialog.c:544-548` | Works around a real TOS bug: `TOUCHEXIT` buttons fire on mouse-down, so `form_alert()` must not appear until the button is physically released |
| `main.c`'s `appl_init()`/`appl_exit()`/`graf_mouse()` sequence | `main.c:24-68` | Standard GEM app bootstrap |
| The low-res guard's `form_alert()` (minus the Setscreen+reset offer) | `main.c:47-57` | §3.3 |
| `form_alert()` `"[icon][line1|line2][btn1|btn2]"` string-building convention | pervasive | Generic GEM idiom, not SideTNFS-specific |
| Atari-side ROM3 transport primitives: `send_command_start()`, `send_param16/32()`, `send_string_field()`, `read_string_field()`, `wait_for_token()` | `sidetnfs_probe.c:231-325` (SideTNFS-Config) | The proven random-token handshake and string-field endianness handling — directly reusable for any new command |

**B. COPY AND ADAPT**

| From | To (concept) |
|---|---|
| `drives_window_run()` / `ov_dialog_init()` / `ov_refresh_rows()` (`dialog.c:2844-2938, 3144-3203`) | Server-profile list dialog (8 fixed rows → 8 profile nicknames) |
| `tnfs_editor_run()` / `te_dialog_init()` (`dialog.c:1786-2033`) | Server-profile editor (nickname/host/port/mount fields are structurally identical to the existing TNFS drive editor, minus the drive-letter field) |
| `add_disk_type_run()` (`dialog.c:2034-2092`) | Template for any "small popup, pick one of N, Cancel returns -1" screen (e.g. a files-view/dirs-view mode picker, if not done as a plain toggle button) |
| The 5-step dialog-run sequence (§3.2/§3.6) | **Factor into one shared helper this time** — the clearest actual improvement over the source material |
| 2-state toggle-button-text idiom (`update_drive_active_button_text()`, `dialog.c:537-541`) | Any Active/Inactive-style field in the new profile editor |
| The `sh`-conditional row-height shrink (§3.4) | Factor into one shared layout helper, used by every new dialog |
| `sidetnfs_probe.c`'s GET/SET/DELETE/SAVE_DRIVE pattern (`sidetnfs_probe.c:341-418`, SideTNFS-Config) | Direct template for new GET/SET/DELETE/SAVE_PROFILE probe functions |

**C. DO NOT COPY**

- Everything listed in §3.7 (WiFi, RTC, GEMDOS-drive-letter, and status-overview code).
- The legacy `sidecart-configurator-atari` protocol layer (command-as-memory-address-offset,
  double-NUL-terminated flat string lists) — a different cartridge generation's wire format,
  structurally incompatible with the current namespaced `APP_<domain>«8|sub` scheme (§9.4).
- Any of the dead `APP_CONFIGURATOR`/`APP_FLOPPYEMUL` legacy commands (`LIST_FLOPPIES`,
  `LOAD_FLOPPY_RO/RW`, `QUERY_FLOPPY_DB`, `DOWNLOAD_FLOPPY`, `GET_SD_DATA`, `CREATE_FLOPPY`,
  all `FLOPPYEMUL_*`) — defined in `commands.h` but **zero handlers exist anywhere in current
  `romemul/*.c`** (confirmed by grep across the whole tree). These are vestigial; do not assume
  they work or extend them.

---

## 4. Current Cartridge Protocol Overview

### 4.1 Command-ID namespace [FACT]

`romemul/include/commands.h:7-10`:
```c
#define APP_CONFIGURATOR 0x00  // legacy, no live dispatcher (see below)
#define APP_FLOPPYEMUL   0x02  // legacy, no live dispatcher
#define APP_RTCEMUL      0x03  // legacy, no live dispatcher
#define APP_GEMDRVEMUL   0x04  // the only namespace with a live dispatch loop today
```
Command = `(APP « 8) | subcommand`. **`main.c` calls `init_gemdrvemul()` as its sole entry point
and it never returns** — there is no dispatcher anywhere in current firmware for the other three
namespaces' command IDs, confirmed by `grep -rn "case GET_SD_DATA\|case LIST_FLOPPIES\|..." romemul/*.c`
returning nothing.

**`APP_GEMDRVEMUL` subcommand allocation, current state:**

| Range | Status |
|---|---|
| `0x00`–`0x0C` | GEMDOS-relay plumbing (ping, vectors, reentry locks, RTC/network start-stop) |
| `0x0D`–`0x12` | SideTNFS drive-list config (GET_CONFIG_INFO, GET/SET/DELETE_DRIVE, SET_CONFIG_DRIVE, SAVE_CONFIG) |
| `0x13`–`0x15` | WiFi/network config (GET/SET/SAVE_NETWORK_CONFIG) |
| `0x16`–`0x18` | RTC/NTP config (GET/SET/SAVE_RTC_CONFIG) |
| `0x19`–`0x1A` | Debug call-tracing (DGETDRV, FSETDTA) |
| `0x1B` | REBOOT_PICO |
| `0x1C` | CHECK_UPDATE (firmware version check) |
| **`0x1D`–`0x35`** | **FREE — 30 unused codes**, explicitly re-verified free against both `commands.h` and the reference Atari driver's own CMD_* table each time a new block was added historically (`commands.h:75-203` comments) |
| `0x36`, `0x39`–`0x3E`, `0x41`–`0x43`, `0x47`–`0x4F`, `0x56`–`0x57` | GEMDOS-call debug tracing (DFREE, DCREATE, FOPEN, FSFIRST, etc.) |
| `0x81`–`0x8B` | File-buffer/DTA/basepage internals |

**[RECOMMENDATION]** `0x1D`–`0x35` is the obvious home for new floppy-browser commands — it is
the only genuinely free, contiguous, already-proven-free range in the live namespace. Per the
task's explicit instruction, this report does not assign final command IDs — just identifies the
range as the clear candidate.

### 4.2 Wire transport mechanics [FACT]

- ROM3 window: `0xFB0000` (Atari side, `romemul/../SideTNFS-Gemdrive` references `ROM3_BASE`),
  a 64 KB (`0x10000`) memory window. Magic header `0xABCD` at offset 0.
- A command is sent as a series of **address-encoded bus reads**: the Atari issues single-value
  reads at computed offsets (header, command ID, payload size, then a random seed low/high word,
  then parameters/string words) — the *address itself* is the datum, not the byte read back.
- On the Pico, a **DMA interrupt handler** snoops the address bus and latches the command:
  ```c
  // gemdrvemul.c:1440-1454
  void gemdrvemul_dma_irq_handler_lookup_callback(void) {
      uint32_t addr = ...;
      if (addr >= ROM3_START_ADDRESS)
          parse_protocol((uint16_t)(addr & 0xFFFF), handle_protocol_command);
  }
  ```
  `handle_protocol_command` only sets `active_command_id` — it does not execute the handler.
- **Actual processing is a single, flat, synchronous `while(true)` polling loop** inside
  `init_gemdrvemul()` (`gemdrvemul.c:4146` onward): `switch (active_command_id) { ... }`. Every
  case writes its response directly into the ROM3 window, calls `write_random_token()` (echoes the
  seed at offset 0 — the Atari-side completion signal), then resets `active_command_id = 0xFFFF`.
  **A new command arriving while one is still active is silently dropped, not queued**
  (`gemdrvemul.c:1430`, guarded by `if (active_command_id == 0xFFFF)`).
- **All commands are synchronous/blocking**, including the worst case already in production:
  `CHECK_UPDATE` performs a real blocking HTTP round-trip (bounded 15s,
  `SIDETNFS_UPDATE_CHECK_TIMEOUT_MS`) and does not write the completion token until the HTTP call
  returns (`gemdrvemul.c:4895-4936`). There is no async/queued-job pattern anywhere in the live
  command set. The only near-exception is a boot-time-only WiFi-connect wait loop that can be
  interrupted by a `GEMDRVEMUL_CANCEL` sent mid-wait — not a reusable general pattern.

### 4.3 Completion-detection mechanism — two different implementations exist [FACT]

This report found **two distinct token-wait implementations**, used by different Atari-side
components, worth flagging clearly (see also §10's naming-collision note):

- **`SIDETNFS.PRG`'s own transport layer** (`SideTNFS-Config/src/sidetnfs_probe.c:246-261`) polls
  via genuine `Vsync()` calls in a bounded loop:
  ```c
  static int wait_for_token(unsigned long seed, int timeout_sec) {
      long budget = (long)timeout_sec * PAL_VBLS_PER_SEC;
      while (budget > 0 && echoed != seed) { Vsync(); echoed = rom3_read_long(...); budget--; }
      return echoed == seed;
  }
  ```
- **`GEMDRIVE.BIN`'s low-level per-GEMDOS-call relay** (`SideTNFS-Gemdrive/src/inc/sidecart_functions.s:200-213`)
  instead uses a raw, fixed-iteration-count (`$000FFFFF`) busy-wait spin loop copied onto the stack
  and executed from RAM, **not** `Vsync()`.

**[RECOMMENDATION]** `FLOPPY.PRG`, being a GEM app exactly like `SIDETNFS.PRG`, should reuse
`SIDETNFS.PRG`'s own `Vsync()`-based `sidetnfs_probe.c` transport pattern (§3.8, list A) — not
`GEMDRIVE.BIN`'s spin-loop, which belongs to a different layer (the boot-time driver) with
different real-time constraints.

### 4.4 ROM3 shared-memory layout and headroom [FACT]

Full offset chain in `romemul/include/gemdrvemul.h:88-332` (verified by hand against every
`_Static_assert`). Highlights:

| Region | Offset | Size |
|---|---|---|
| Handshake header (token, seed, timeout, ping/RTC/network status) | 0–67 | 68 B |
| `GEMDRVEMUL_DEFAULT_PATH` | 68 | 128 B |
| `GEMDRVEMUL_DTA_TRANSFER` (one GEMDOS DTA) | 200 | 44 B |
| **`GEMDRVEMUL_READ_BUFF`** (file-read staging buffer) | 264 | **16384 B** |
| various file/exec/pexec status fields | 16648–17047 | ~400 B |
| `GEMDRVEMUL_SIDETNFS_CONFIG` (GET_CONFIG_INFO) | 17304 / `0x4398` | 20 B |
| `GEMDRVEMUL_SIDETNFS_DRIVE` (drive record) | 17324 | 198 B |
| `GEMDRVEMUL_SIDETNFS_NETWORK` | 17524 / `0x4474` | 182 B |
| `GEMDRVEMUL_SIDETNFS_RTC` | 17704 / `0x4528` | 74 B |
| `GEMDRVEMUL_SIDETNFS_UPDATE` (CHECK_UPDATE) | 17780 / `0x4574` | 36 B, **ends at 17816 / `0x4598`** |

**Free space above the last used offset: 65536 − 17816 = 47,720 bytes ≈ 46.6 KB, all contiguous.**

A **separate** 64 KB window, ROM4 (`0xFA0000` Atari-side / `0x20020000` Pico-side), holds the
emulated GEMDOS-trap firmware/vector code itself (`ROM_IN_RAM`) — it is not exchange data and is
**not** available as scratch space for new protocol fields.

### 4.5 Payload size limits and alignment [FACT]

- **Request-payload channel cap**: `MAX_PROTOCOL_PAYLOAD_SIZE = 2048 + 64 = 2112` bytes
  (`romemul/include/tprotocol.h:21`) — the ceiling for anything sent via the address-encoded
  parameter/string channel (a single `malloc(2112)` on the Pico side, `tprotocol.c:68`).
- **Largest existing single data field**: `GEMDRVEMUL_READ_BUFF`, 16384 bytes — but this lives
  directly at a fixed ROM3 offset and is read by the Atari with plain memory reads, **not** through
  the 2112-byte encoded-payload channel. This is the important asymmetry: request payloads are
  capped at ~2 KB, but response *data placed directly in the shared window* is bounded only by
  the window's own free space (~46.6 KB, §4.4).
- **Alignment rules, enforced by compile-time `_Static_assert`s throughout `gemdrvemul.h`**:
  `uint32_t` fields (`WRITE_AND_SWAP_LONGWORD`) must be 4-byte aligned; `uint16_t` fields
  (`WRITE_WORD`) must be 2-byte aligned; every string field's byte length must be even
  (`CHANGE_ENDIANESS_BLOCK16` processes them as whole 16-bit words). A real hardware HardFault
  from an unaligned 32-bit Cortex-M0+ store is the documented reason `SIDETNFS_NETWORK_ALIGN4()`
  exists (`gemdrvemul.h:234-240`) — this is not theoretical, it was hit on real hardware.

### 4.6 Pico RAM budget [FACT]

Linker regions (`romemul/memmap_romemul.ld:24-40`): `RAM` 128 KB + `SCRATCH_X` 4 KB + `SCRATCH_Y`
4 KB + `ROM_IN_RAM` 128 KB = 264 KB total (matches RP2040's actual on-chip SRAM). `ROM_IN_RAM` is
entirely consumed by the ROM3+ROM4 windows — not general-purpose space.

**Measured from the actual build** (`arm-none-eabi-size` on `romemul.elf`): `.data`+`.bss` end at
`0x2001c080` within the 128 KB `RAM` region (`0x20000000`–`0x20020000`) — **≈114.5 KB used, ≈15.9 KB
free** general-purpose RAM.

**[INFERENCE]** A naive 6.4 KB directory-page buffer allocated as ordinary Pico static/heap RAM
would consume ~40% of that remaining ~16 KB headroom. The existing precedent for "a chunky
per-request data block" (`GEMDRVEMUL_READ_BUFF`, 16 KB) is instead placed directly in the ROM3
window, which has ~46.6 KB free. **See §7 for the full memory-impact analysis.**

### 4.7 No existing paging/batching mechanism [FACT]

Confirmed dead code, not extendable: the legacy `APP_CONFIGURATOR`/`APP_FLOPPYEMUL` commands
(`LIST_FLOPPIES`, `QUERY_FLOPPY_DB`, `LOAD_FLOPPY_RO/RW`, `GET_SD_DATA`, `DOWNLOAD_FLOPPY`,
`CREATE_FLOPPY`, all `FLOPPYEMUL_*`) have zero handlers anywhere in `romemul/*.c` (§4.1). The only
"chunking" precedent that is actually live is the single-item file-data read/write loop (16 KB /
2 KB chunks per round trip, driven by a repeated `CMD_READ_BUFF_CALL` loop on the Atari side,
`SideTNFS-Gemdrive/src/gemdrive.s:1548-1619`) and the existing `Fsfirst`/`Fsnext` pair, which
returns **exactly one directory entry (one 44-byte DTA) per round trip** — today's directory
listing primitive has no batching of multiple entries into one response at all. This is the
concrete baseline any new 25-entry page command must depart from (see §10's open question on
pagination cost).

---

## 5. TNFS Directory / LFN Data Flow

This is the most safety-critical piece of research for the whole project: it determines whether a
"get the original filename" browser API is even possible without inventing new TNFS-server-side
behavior. **It is possible — the data exists — but only for one function call today.**

### 5.1 Where the raw TNFS filename first appears [FACT]

There is no separate "TNFS client library" — the TNFS wire protocol is implemented directly inside
`romemul/sidetnfs_probe.c` (~4400 lines; **note this is a different file from, but shares an
identical name with,** `SideTNFS-Config/src/sidetnfs_probe.c` — see §10). The live Fsfirst/Fsnext
path sends one `TNFS_CMD_READDIRX` (`0x18`) per directory entry
(`SIDETNFS_READDIRX_MAX_ENTRIES=1`) and parses the response in `fslisting_parse_batch()`
(`sidetnfs_probe.c:4016-4073`). The raw wire filename is first materialized at:

```c
// sidetnfs_probe.c:4039
const char *name = (const char *)&buf[needle + 13];
```

`buf` is the live UDP receive buffer (`SIDETNFS_RX_BUF_SIZE 256`, `sidetnfs_probe.c:262`) — a
direct pointer into the just-received datagram, exactly as the TNFS server sent it: NUL-terminated,
case-sensitive, arbitrary length (up to ~240 usable bytes given the 256-byte buffer and its
header). **This is the earliest, and only, point in the firmware where the true, unfiltered LFN
exists.**

### 5.2 What happens next — rejection, not truncation [FACT]

```c
// sidetnfs_probe.c:4052
bool normalized_ok = sidetnfs_normalize_dir_entry(name, flags, size, mtime, &out_entries[count]);
```

`sidetnfs_normalize_dir_entry()` (`sidetnfs_probe.c:1925-1966`) calls
`sidetnfs_is_supported_83_name()` (`sidetnfs_probe.c:1837-1894`) — a **strict rejector**, not a
mangler: uppercase-only, single dot, base ≤8 chars, extension ≤3 chars, no FAT-invalid characters,
no leading dot. The header comment is explicit: lowercase/long names are *"intentionally ignored —
rename them uppercase on the TNFS server instead of relaxing this check."* Anything that isn't
already a valid 8.3 name — **including any genuine long filename** — is skipped entirely and never
appears in the Atari's directory listing at all. There is no shortening/aliasing on this path
today, only silent omission.

The result struct is `SidetnfsAtariDirEntry.name[14]` (`sidetnfs_probe.h:423-432`) — 8.3-only by
construction, comment: *"No long-name↔short-name mapping table exists yet."*

### 5.3 The rest of the chain — no further transformation [FACT]

`gemdrvemul.c`'s `GEMDRVEMUL_FSFIRST_CALL`/`FSNEXT_CALL` handlers call
`populate_dta_from_sidetnfs_entry()` (`gemdrvemul.c:637-672`), which copies the already-8.3
14-byte name byte-for-byte into the 44-byte DTA transfer area (`GEMDRVEMUL_DTA_TRANSFER`, ROM3
offset 200). The Atari driver (`SideTNFS-Gemdrive/src/gemdrive.s:1818-1829`,
`.populate_fsdta_struct_loop`) does a raw 44-byte copy into the caller's real DTA. Confirmed
explicitly: *"the 44-byte GEMDOS DTA... is also exactly the name Fsnext/Fsfirst returns to the
Atari — there is no further transformation after that point"* (`sidetnfs_probe.h:1855-1860`).

### 5.4 The one place raw LFN and 8.3 name coexist [FACT — the critical finding]

Inside `fslisting_parse_batch()`, between line 4039 (`name`, raw) and line 4052
(`out_entries[count]`, 8.3-filtered), both values are simultaneously in scope in the same stack
frame. **This is the natural, and currently only, hook point for a "capture the original LFN"
command.**

Caveats, all proven from code:
- This coexistence lasts for one loop iteration only. `buf` (the UDP receive buffer) is reused on
  the very next READDIRX round trip — nothing downstream retains the raw name.
- A diagnostic-only trace facility exists (`SidetnfsNameTraceEvent`,
  `sidetnfs_probe.h:1862-1879`), but its own `raw_name` field is `char[14]` — it **also**
  truncates, is compiled out by default (`#if SIDETNFS_DIAG_DUMP_ON_SELECT`), and only dumps to an
  SD file — not usable as a live API in any form.
- No other point in the call chain (the root-probe diagnostic path, `gemdrvemul.c`) ever sees a
  raw/full-length name again.

### 5.5 A ready-to-integrate LFN module already exists [FACT]

`../long-filenames/tests/host_lfn/sidetnfs_lfn.c` (2168 lines) + `.h` (649 lines). **Not
integrated anywhere** — `grep -rn "sidetnfs_lfn" SideTNFS-Firmware` returns zero matches. The
prototype's own `README.md:12-14` states explicitly: *"Nothing here is integrated into the
firmware. `sidetnfs_lfn.c` and `sidetnfs_lfn.h` are meant to be dropped into `romemul/` later
without changes."*

Its design is well-matched to the hook point above: plain C11, zero OS/Pico/lwIP/TNFS
dependencies, no `malloc` (caller-supplied buffers), and a `sidetnfs_lfn_scanner_t` callback
abstraction (`open`/`next`/`close`) explicitly meant to sit "on top of opendir/readdir/closedir"
— i.e. directly on the exact `sidetnfs_probe.c` plumbing described above, receiving the raw `name`
before it would otherwise be discarded. Public API of note:

```c
sidetnfs_lfn_status_t sidetnfs_lfn_dirmap_build(sidetnfs_lfn_dirmap_t *map, int slot, const char *dir_path,
                                                 const sidetnfs_lfn_scanner_t *scanner, void *user, uint32_t now_seconds);
sidetnfs_lfn_status_t sidetnfs_lfn_dirmap_entry(const sidetnfs_lfn_dirmap_t *map, size_t index,
                                                 char *out_alias, size_t alias_cap, char *out_name, size_t name_cap, bool *out_is_dir);
sidetnfs_lfn_status_t sidetnfs_lfn_dirmap_find_name(const sidetnfs_lfn_dirmap_t *map, const char *long_name,
                                                     char *out_alias, size_t alias_cap);   // LFN -> alias reverse lookup
sidetnfs_lfn_status_t sidetnfs_lfn_resolve_path(...);   // full N:\...\ALIAS path -> exact long TNFS path
```

It maintains a bidirectional alias↔long-name mapping per directory plus a small TTL cache
(`SIDETNFS_LFN_CACHE_ENTRIES=32`, `SIDETNFS_LFN_CACHE_TTL_SECONDS=60`) — materially more capable
than today's reject-only filter, since it *aliases* rather than *drops* non-8.3 names.

**[RECOMMENDATION]** The browser feature does not need this prototype's full aliasing/GEMDOS-LFN
machinery (the task explicitly says "bypass GEMDOS 8.3 entirely, no LFN filesystem support"). But
its `sidetnfs_lfn_dirmap_build()`/`_entry()` pair — build a directory's entry list once, retrieve
entries by index — is *exactly* the shape a paginated browser page needs, and its
scanner-callback design already anticipates being fed from `fslisting_parse_batch()`'s raw `name`.
Reusing (or closely modeling) this module for the browser's directory-listing cache is a strong,
concrete starting point for Phase 2, rather than inventing a new capture mechanism from scratch.

### 5.6 Atari-side filename limits [FACT]

`SideTNFS-Gemdrive/src/gemdrive.s` has no filename-length logic of its own — the 14-byte limit is
inherited entirely from the standard GEMDOS `DTA.d_fname` field (a TOS convention, not a SideTNFS
addition), confirmed by both the driver's raw 44-byte DTA copy loops and
`gemdrive-test/src/tests_readonly.c:89-91` declaring `char preferred_name[14]` when consuming
Fsfirst/Fsnext results.

### 5.7 No existing GEMDOS LFN extension anywhere [FACT]

No custom Fsfirst/Fsnext "long" variant opcode, no LFN-flavored XFS/cookie extension, exists in
any of `SideTNFS-Gemdrive`, `gemdrive-test`, or `SideTNFS-Firmware` (all "LFN" hits in
`romemul/` are FatFS's own unrelated third-party config comments). Confirms the task's premise
that GEMDOS LFN must be bypassed entirely, not extended.

### 5.8 A second, independent coexistence point exists on the SD/FatFS path [FACT, out of stated scope but worth flagging]

Separately from TNFS, the SD-card backend (`romemul/filesys.c:1008-1310`) does real 8.3
**mangling** (`shorten_fname()`, classic `NAME~1.EXT`) rather than rejection. FatFS's own
`FILINFO` struct (with `FF_USE_LFN=3`) already provides **both** `fname` (long, 256B) and
`altname` (short, 13B) simultaneously from `f_findfirst`/`f_findnext` — but current code only
reads `fname` and manually shortens it, never touching the ready-made `altname`. This is a second,
structurally different LFN/8.3 coexistence point. **[OPEN QUESTION]** Since the stated scope is
TNFS network sources only (no SD, no drive letters), this is likely out of scope for `FLOPPY.PRG`
— flagged here only so it isn't mistaken for part of the same code path later.

---

## 6. Proposed 25-Entry LFN Browser Architecture [RECOMMENDATION]

Everything in this section is a proposal for the next phase, not a description of existing code.

- **Hook point**: capture the raw `name` inside (or immediately alongside)
  `fslisting_parse_batch()` at `sidetnfs_probe.c:4039`, before/instead of routing it through
  `sidetnfs_normalize_dir_entry()`'s reject-only filter (§5.4).
- **Retention mechanism**: build a per-directory listing cache modeled on (or reusing)
  `../long-filenames`'s `sidetnfs_lfn_dirmap_t` design (§5.5) — populated when the browser opens
  a directory, sliced into pages on request, discarded when the directory changes.
- **Session model**: one active browse session (current directory string, current page/mode)
  held in Pico RAM — not flash, not multi-session. Matches the task's explicit "one active
  directory" constraint.
- **Two view modes per directory** (`.ST` files vs. subdirectories, never mixed, switched by a
  button): the TNFS `READDIRX` response's existing `flags` byte (already passed into
  `sidetnfs_normalize_dir_entry()` today) is the natural place to distinguish files from
  directories server-side, so this split can be done as the listing is built rather than requiring
  two separate full directory walks — but see §10's open question on how this interacts with the
  one-entry-per-round-trip cost of TNFS `READDIRX` today.
- **No per-entry type field needed**, exactly as specified: since files and directories are never
  returned on the same page, the mode is a property of the *page request*, not the entry.
- **Page buffer placement**: build the response page directly in the ROM3 shared-memory window's
  free tail (above offset `0x4598`, §4.4/§4.6), following the existing `GEMDRVEMUL_READ_BUFF`
  precedent — not as a new Pico general-RAM allocation, which only has ~16 KB of headroom (§7).

---

## 7. Memory Impact

Reproducing and extending the task's own example with hard numbers from §4.4/§4.6:

```
current directory:          256 bytes
25 entries × 256 bytes:    6400 bytes
page metadata (index, entry count, mode, status):  ~16-32 bytes
                            ------------------
Total per-page payload:    ~6.4-6.5 KB
```

**Where should this live?**

| Location | Headroom available | Assessment |
|---|---|---|
| Pico general-purpose RAM | **~15.9 KB free** (measured, §4.6) | A 6.4 KB static/heap buffer here would consume ~40% of all remaining general RAM. **Not recommended.** |
| Pico ROM3 shared-memory window | **~46.6 KB free** (§4.4) | 6.4 KB is ~14% of the free tail. Matches the existing `GEMDRVEMUL_READ_BUFF` (16 KB) precedent of placing large single-request data blocks directly in this window. **[RECOMMENDATION] Preferred location.** |
| Atari RAM | Ample on any 1 MB+ ST — several hundred KB of free TPA is typical (general domain knowledge, not from either repo — no "typical free ST RAM" constant exists in this codebase) | Not a constraint; the Atari side copies the page out of the ROM3 window into its own buffer exactly as it already does for file reads. |

**[RECOMMENDATION]** Build the page once, directly in the Pico's ROM3 exchange-window free tail;
the Atari reads/copies it out as needed, exactly like `GEMDRVEMUL_READ_BUFF` today. Do not hold a
second, redundant full-page copy in Pico general RAM — the ROM3 window copy *is* the buffer, the
same way `GEMDRVEMUL_READ_BUFF` is not duplicated elsewhere.

**[OPEN QUESTION]** The exact wire mechanics of *how* the Atari triggers the page build and reads
it back (address-encoded request + plain-read response, vs. something else) is an implementation
detail for the protocol-design sub-phase, not resolved here — see §9's explicit note that the
2112-byte request-payload cap (§4.5) applies only to the *request* side, not to where response data
is placed.

---

## 8. Proposed 8-Profile Floppy Server Configuration

### 8.1 Existing flash infrastructure — the precedent is already established twice [FACT]

| Sector | Absolute offset | Size | Magic | Version | Purpose |
|---|---|---|---|---|---|
| `CONFIG_FLASH` (legacy) | `0x100DE000` | 8 KB | `0x12340000\|CONFIG_VERSION` | 2 | WiFi/RTC/misc key-value (48 entries max). **Documented as unsafe to shrink/reorder on live devices** — several `PARAM_*` entries are explicitly kept only to preserve array-offset compatibility (`config.h:39-43`). |
| `ROM_FLASH` | `0x100E0000` | 128 KB | n/a | n/a | Embedded `SIDETNFS.PRG`/`README.TXT` + `GEMDRIVE.BIN` images |
| `SIDETNFS_CONFIG_FLASH` (drives) | `0x10100000` | 4 KB | `"STDF"` | 3 (migrates from 2) | 8-slot GEMDOS drive list, CRC32-protected |
| `SIDETNFS_SYSTEM_CONFIG_FLASH` | `0x10101000` | 4 KB | `"SYSC"` | 1 | WiFi + Network + RTC/NTP, CRC32-protected, replaces the legacy store for these fields |

Both `SIDETNFS_*` sectors follow an **identical, proven contract**:
`init()` cascades magic → version(+migration if applicable) → CRC32 → structural validation →
**wholesale** fallback to built-in defaults on any failure (never a partial/salvaged record).
`save()` validates → builds a zeroed "clean" image → computes CRC → disables interrupts only
around the erase+program pair → erases the whole sector → programs a page-rounded (256B) prefix →
restores interrupts → reads back via XIP → re-verifies magic/version/CRC, returning distinct
`FLASH_WRITE_FAILED` vs. `CRC_MISMATCH` codes. Flash is only ever written on an explicit
"Save" command — everything else is RAM-only, bounding flash wear to one erase/program cycle per
user save.

`sidetnfs_system_config.h`'s own header comment (`:24-32`) documents the convention used to place
it: *"the next free 4KB-aligned sector right after the existing SIDETNFS drive-config sector."*

### 8.2 Spare flash budget [FACT]

Total `FLASH` linker region: `0x10000000`–`0x101DE000` (1912 KB). Firmware code/data occupies up
to `0x100DE000` (asserted by the linker; actual usage ~540 KB per the firmware's own protocol doc).
Reserved sectors run through `0x10102000` (end of `SIDETNFS_SYSTEM_CONFIG_FLASH`). **Free space
from `0x10102000` to the end of the flash region: `0x101DE000 − 0x10102000 = 0xDC000 ≈ 880 KB`,**
completely untouched.

**[RECOMMENDATION]** Next free 4 KB-aligned offset for a new sector: **`0x10102000`**, following
the identical convention `SIDETNFS_SYSTEM_CONFIG_FLASH` used to claim its own offset.

### 8.3 Linker-enforcement gap — a risk to avoid repeating [FACT + RECOMMENDATION]

`CONFIG_FLASH`/`ROM_FLASH`/`SIDETNFS_CONFIG_FLASH` are declared as actual `MEMORY` regions in
`memmap_romemul.ld`, purely as documentation/reservation markers (the linker script's own comment,
`:33-38`, says none of them are referenced by any `SECTIONS` output). **But
`SIDETNFS_SYSTEM_CONFIG_FLASH` is not in the linker script at all** — its non-overlap proof rests
entirely on a hand-written comment in `sidetnfs_system_config.h`, not a linker check. **[RECOMMENDATION]**
A new floppy-profile sector should be added as an actual `MEMORY` entry (matching
`SIDETNFS_CONFIG_FLASH`'s stronger precedent, not `SIDETNFS_SYSTEM_CONFIG_FLASH`'s weaker one) to
get linker-level collision protection instead of relying solely on a comment.

### 8.4 A near-identical 8-profile model was already designed once, and abandoned [FACT — strong precedent]

`docs/sidetnfs-config-protocol.md:15-16` (translated from Dutch): *"This fully replaces the Phase
9B2 model (max. 8 servers, no config drive) — that model was never committed."* An earlier,
unshipped design already had almost exactly the shape this task needs (a flat list of up to 8
server records, no GEMDOS drive-letter concept) before the team pivoted to the current
drive-letter-oriented model. **[INFERENCE]** No source states *why* it was abandoned beyond
"replaced" — genuinely undeterminable from the repo (**[OPEN QUESTION]**) — but its existence is
strong evidence that an 8-profile record shape fits this codebase's own conventions comfortably,
and that record sizing precedent (`sidetnfs_drive_config_t` = 192 bytes for a much richer record
than a floppy profile needs) means 8 floppy profiles will be well under 1 KB total, let alone the
4 KB a sector budget would allow.

### 8.5 Proposed record shape [RECOMMENDATION]

Following the field-length conventions already established (`SIDETNFS_NICKNAME_LEN`=24,
`SIDETNFS_HOST_LEN`=64, `SIDETNFS_MOUNTPATH_LEN`=32):

```c
typedef struct {
    uint8_t  state;              // EMPTY / DISABLED / ENABLED, mirrors sidetnfs_drive_slot_state_t
    uint8_t  reserved[3];
    char     nickname[24];       // matches SIDETNFS_NICKNAME_LEN
    char     host[64];           // matches SIDETNFS_HOST_LEN
    uint16_t port;
    char     mount_path[32];     // matches SIDETNFS_MOUNTPATH_LEN
    char     last_directory[256]; // per the task's explicit browser CWD size
} floppy_profile_t;              // ~380 bytes

typedef struct {
    uint32_t magic;               // new 4-byte tag, e.g. "FLPP"
    uint32_t version;
    uint8_t  active_profile_index; // last-active profile, restored on FLOPPY.PRG startup
    uint8_t  reserved[3];
    floppy_profile_t profiles[8];  // ~3.1 KB total
    uint32_t crc32;
} floppy_config_flash_t;          // ~3.1 KB, fits a 4KB sector with room to spare
```

**Explicitly independent from `drive.h`/`sidetnfs_config.h`'s `Drive`/`DriveConfig` structs** — no
shared struct, no drive-letter field, no shared flash sector, matching both the task's explicit
requirement and this codebase's own one-concern-per-sector convention (§8.1).

### 8.6 Forward/backward compatibility [RECOMMENDATION]

Reuse the exact cascade proven twice already (§8.1): magic → version (+ migration path, reserved
for future use, none needed at v1) → CRC32 → structural validation → wholesale fallback to
built-in defaults. No firmware/build-hash check, matching the existing policy
(`sidetnfs_system_config.h:112-113`).

---

## 9. Minimal Proposed Protocol Extensions [RECOMMENDATION — none of this is implemented]

Per the task's instruction, command IDs below are illustrative placeholders from the confirmed
free range (§4.1), not final assignments.

### 9.1 Profile management (mirrors the existing drive-list command shape exactly)

| Illustrative ID | Command | Request | Response |
|---|---|---|---|
| `0x1D` | GET_PROFILE | `uint32_t index` (0-7) | full profile record + status |
| `0x1E` | SET_PROFILE | `uint32_t index` + profile fields | status only (RAM-only, like `SET_DRIVE`) |
| `0x1F` | DELETE_PROFILE | `uint32_t index` | status only |
| `0x20` | SET_ACTIVE_PROFILE | `uint32_t index` | status only |
| `0x21` | SAVE_PROFILES | none | status only (the only command that writes flash, exactly like `SAVE_CONFIG`) |

### 9.2 Directory browsing

| Illustrative ID | Command | Request | Response |
|---|---|---|---|
| `0x22` | BROWSE_OPEN_DIR | path (up to 256B, within the 2112B request cap) | status |
| `0x23` | BROWSE_GET_PAGE | page index + mode (files/dirs) | status, entry_count (≤25), page data (built directly in the ROM3 window's free tail per §7) |
| `0x24` | BROWSE_SELECT_FILE | directory + filename (as the Atari already holds it, no ID/handle system per the task's explicit instruction) | status (simple confirmation back to the Pico) |

### 9.3 Design notes

- **Pagination strategy**: offset/page-index based (explicit `page_index` parameter), not a
  next/prev-only cursor — simplest fit for "up to 25 entries, no 8.3, no handles" as specified.
  **[OPEN QUESTION]** Whether the Pico should re-walk the TNFS directory from scratch per page
  request, or hold one open TNFS session/cursor across `BROWSE_GET_PAGE` calls, is a real
  engineering trade-off (latency vs. held server-side state) not resolved by this research — see
  §10.
- **Data ownership**: the Pico owns the live TNFS session, the raw-LFN capture (§5/§6), and the
  page cache. The Atari (`FLOPPY.PRG`) owns display/selection state and the persisted 8-profile
  list, which round-trips through GET/SET/SAVE commands exactly like the existing drive config —
  **no local config file on the Atari side**, matching `SIDETNFS.PRG`'s own documented policy
  (`SideTNFS-Config/README.md:122-129`: *"There is no local config file... configuration is read
  entirely from the firmware's RAM/flash config."*).
- **Where state lives**: current directory + page cache → Pico RAM/ROM3 window only, session-scoped,
  rebuilt from the profile's `last_directory` field each time `FLOPPY.PRG` starts. The 8 profiles +
  active-profile index + each profile's `last_directory` → Pico flash (§8), persisted only on
  explicit Save.
- **Request-side size discipline**: any new request payload (e.g. a 256-byte path) must fit within
  the existing `MAX_PROTOCOL_PAYLOAD_SIZE` (2112 bytes, §4.5) — comfortably true for a single path
  string, with room to spare.

---

## 10. Major Risks / Open Questions

1. **[OPEN QUESTION — real engineering cost]** Building a 25-entry page today would require either
   (a) 25 sequential TNFS `READDIRX` round-trips inside one Pico-side command handler (bounded and
   synchronous, but each round-trip is a real UDP-over-WiFi exchange with real latency), or (b)
   changing `SIDETNFS_READDIRX_MAX_ENTRIES` from 1 to something larger and enlarging the 256-byte
   RX buffer accordingly — a genuine TNFS-protocol-level and buffer-sizing change, not just a new
   command. This is unresolved and should be the subject of a small firmware-only latency spike
   before committing to any Atari-side UI timing assumptions.
2. **[FACT, flagged as a documentation risk]** Two files in two different repositories are both
   named `sidetnfs_probe.c` and do genuinely different things: `SideTNFS-Config/src/sidetnfs_probe.c`
   is the **Atari-side** ROM3 transport client (C, `Vsync()`-based polling); `SideTNFS-Firmware/romemul/sidetnfs_probe.c`
   is the **Pico-side** TNFS wire client (C, runs on the RP2040). Keep this distinction explicit in
   any future design doc or code review — the identical filename is a real source of confusion.
3. **[FACT]** RAM pressure on the Pico is real and measured: only ~15.9 KB of general-purpose RAM
   is free in the current build. Any new firmware feature (not just the browser) needs to be
   conscious of this — the ROM3 window, not Pico heap, is the natural home for bulk data (§7).
4. **[FACT]** The command-ID range `0x1D`–`0x35` is free *today*, first-come-first-served against
   any other concurrent firmware work — worth claiming/documenting promptly. (Noted only as a
   process risk: this environment shows another active session apparently working in
   `SideTNFS-Gemdrive`; if that work also touches `commands.h`, coordinate to avoid a collision.)
5. **[FACT]** The legacy `APP_CONFIGURATOR`/`APP_FLOPPYEMUL` floppy-related command IDs
   (`LIST_FLOPPIES`, `LOAD_FLOPPY_RO/RW`, `QUERY_FLOPPY_DB`, `DOWNLOAD_FLOPPY`, `GET_SD_DATA`,
   `CREATE_FLOPPY`) exist in `commands.h` but are **dead code with zero handlers** in current
   firmware. Do not assume any of it is functional groundwork to build on.
6. **[FACT]** The original TNFS LFN is proven available before GEMDOS filtering, but only for the
   duration of one function call, and the receive buffer that carries it is only 256 bytes total
   (not 256 bytes just for the name) — very long TNFS paths near the theoretical maximum have not
   been verified to fit. This should be tested with real long filenames in the next phase.
7. **[FACT]** `SIDETNFS_SYSTEM_CONFIG_FLASH`'s flash-sector reservation is proven free only by a
   hand-written comment, not a linker check — the same risk would apply to a new floppy-profile
   sector unless it's added to the linker `MEMORY` block (§8.3).
8. **[FACT]** A second, structurally different LFN/8.3 coexistence point exists on the SD/FatFS
   backend (§5.8) — out of the stated TNFS-only scope, but a source of confusion if anyone later
   assumes "the LFN work" covers both backends uniformly.
9. **[FACT]** The old `sidecart-configurator-atari` protocol (command-as-memory-address-offset, flat
   double-NUL-terminated string lists) is a different cartridge generation and is **not**
   wire-compatible with the current namespaced protocol — only its UI *pattern* (one generic pager
   reused across three screens) is a valid reuse-in-spirit source, never its code or constants.
10. **[OPEN QUESTION]** No selectable-list-plus-action-buttons UI pattern exists anywhere in
    `SIDETNFS.PRG` to draw from for the file-browser dialog specifically — only the "8
    always-visible rows, each with an inline button" pattern from the Drives dialog. Whether that
    shape suits a 25-row paginated file list, or a different pattern is needed, is a UI design
    decision for Phase 3, not something existing code settles.

---

## 11. Recommended Implementation Order for Steps 1–4

This follows the user's own stated roadmap, sequenced against the dependencies this research
surfaced.

### Step 1 — FLOPPY.PRG skeleton + server profiles
- Mirror `SideTNFS-Config`'s build setup (Makefile, `m68k-atari-mint-gcc` toolchain).
- Copy `main.c`'s `appl_init()`/`appl_exit()`/`graf_mouse()` bootstrap and the low-res guard
  (§3.3), **dropping** the Setscreen+reset offer per the task's explicit brief.
- Copy the generic GEM plumbing verbatim (§3.8, list A): `set_obj()`, `wire_tree()`, `init_ti()`,
  `buf_*` helpers, `wait_mouse_release()`.
- **Unlike `SIDETNFS.PRG`, factor the 5-step "run a modal dialog" sequence into one shared helper
  from the start** (§3.6) — this app will have several dialogs and the source material never did
  this itself.
- Implement the new flash sector (§8) and profile commands (§9.1) on the firmware side in
  parallel.
- Build the Server selector ([S]/[Server] → up to 8 nicknames → select/connect; "Edit servers..."
  → profile editor), adapting `drives_window_run()`/`ov_dialog_init()` and
  `tnfs_editor_run()`/`te_dialog_init()` (§3.8, list B) as the concrete templates.

### Step 2 — LFN directory browsing protocol
- Firmware: hook `fslisting_parse_batch()` (§5.4) to capture the raw name; integrate or
  reimplement the `../long-filenames` prototype's dirmap for per-directory listing storage (§5.5).
- **Resolve the pagination-cost open question first** (§10.1) — spike the real round-trip latency
  of building a 25-entry page before finalizing the Atari-side UI's assumptions about
  responsiveness.
- Implement `BROWSE_OPEN_DIR`/`BROWSE_GET_PAGE` (§9.2), building each page directly in the ROM3
  window's free tail (§7).

### Step 3 — Custom File Selector with LFN
- Files-view/directories-view toggle button, up to 25-row page (with page-forward/back rather than
  scrolling, consistent with the fixed-page-size design).
- Long filenames (up to 256 bytes) will not fit an 80-column screen — this dialog needs its own
  display treatment (truncate with a way to reveal the full name, e.g. on selection) that has no
  existing precedent in `SIDETNFS.PRG` to copy from; treat as new design work.
- Reuse the `OV_*` row-list layout mechanics and `buf_copy()`/`set_buf()` string helpers (§3.8).

### Step 4 — Confirmation of selected file
- Smallest step: a `form_alert()`-style confirmation echoing the chosen directory + filename, then
  `BROWSE_SELECT_FILE` (§9.2) sends it back to the Pico exactly as specified — no ID/handle system.
- Natural stopping point before any floppy-mounting/image-validation work, which remains
  explicitly out of scope.
