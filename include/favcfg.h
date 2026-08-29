#ifndef FAVCFG_H
#define FAVCFG_H

#include "profile.h"

/* FLOPPY.PRG's favorites persistence -- deliberately plain-text, fixed
 * line order, no keywords/headers/hashes/binary records. See
 * src/favcfg.c's own file header for the full rationale and on-disk
 * layout; this header only exposes the small set of operations dialog.c
 * needs.
 *
 * Buffer sizes match the browse protocol's own limits (floppy_probe.h's
 * FLOPPY_BROWSE_NAME_LEN/_CWD_LEN) -- full filenames/directories are
 * always kept, never truncated in storage (only the on-screen display
 * may shorten them). */
#define FAVCFG_NAME_LEN 256
#define FAVCFG_DIR_LEN  256

/* Must be called once, before any other favcfg_*() function -- captures
 * FLOPPY.PRG's own directory (the current directory at program launch,
 * per standard GEM/TOS convention) so FLOPPY.CFG is always found next to
 * the program, never wherever the user's current directory happens to
 * be for some unrelated reason. */
void favcfg_init(void);

/* Compares cfg's own 8 mount slots (as just read from the firmware)
 * against CONFIG.CFG's own record of what they were last time. Any slot
 * whose type/server/path/port changed (or that CONFIG.CFG has no record
 * of at all) has its favorites reset (see favcfg_erase_all()) and
 * CONFIG.CFG is brought up to date. Missing CONFIG.CFG, or every slot
 * matching, is not an error and never writes anything. Call once at
 * startup, after cfg has been loaded from the firmware. */
void favcfg_startup_validate(const ProfileConfig *cfg);

/* Reads just the filename for favorite 1..60 of the given mount slot
 * (1..8) into out_name (out_name_cap bytes) -- "" if out of range, the
 * mount slot has no favorites file yet, or the file is shorter than that
 * favorite. */
void favcfg_read_name(int mount_slot, int favorite, char *out_name, int out_name_cap);

/* Reads just the filenames for `count` CONSECUTIVE favorites starting at
 * `first_favorite` (1..60) into out_names[0..count-1] (each
 * FAVCFG_NAME_LEN bytes) -- "" for any that are out of range or past the
 * end of a short/missing file. Opens/reads MOUNTn.CFG exactly ONCE for
 * the whole batch, unlike calling favcfg_read_name() in a loop -- built
 * for building one visible page (dialog.c's FA_ROWS favorites) at once,
 * since on a TNFS-backed drive each separate file open has its own real
 * network cost. */
void favcfg_read_page_names(int mount_slot, int first_favorite, int count,
                             char out_names[][FAVCFG_NAME_LEN]);

/* Reads both the filename and directory for favorite 1..60 -- used only
 * when a single favorite is actually selected/started/moved, never for a
 * whole page at once. */
void favcfg_read_entry(int mount_slot, int favorite,
                        char *out_name, int out_name_cap,
                        char *out_dir, int out_dir_cap);

/* Writes favorite `favorite`'s name+dir via a streaming rewrite (see
 * src/favcfg.c), saving immediately. name/dir may be empty strings to
 * clear the slot (same effect as favcfg_erase_entry()). Shows a GEM
 * alert only on a genuine write failure -- never for a missing/short
 * input file. */
void favcfg_write_entry(int mount_slot, int favorite, const char *name, const char *dir);

/* Clears favorite `favorite` (both lines empty), saving immediately. */
void favcfg_erase_entry(int mount_slot, int favorite);

/* Clears every favorite (all 60) for the given mount slot, saving
 * immediately -- used by startup mount-change invalidation and by the
 * user's own "erase all" confirmation. */
void favcfg_erase_all(int mount_slot);

/* Swaps favorite `from` and favorite `to` in one streaming rewrite pass
 * (so if `to` was empty, `from` becomes empty too -- nothing is lost
 * either way), saving immediately. A no-op if from == to. */
void favcfg_move_entry(int mount_slot, int from, int to);

/* Reads the last mount slot (1..8) the user actively selected via
 * [Source] -- independent of whatever the firmware itself reports as its
 * own active profile, since picking a source in this app is deliberately
 * an in-memory-only choice until an explicit profile Save (see
 * perform_save()'s own comment in dialog.c) -- so without this, the
 * choice never survived a restart. Returns 0 (not 1..8) if the file is
 * missing or its content is out of range; the caller must still check
 * the slot is actually configured before trusting it, same as any other
 * mount slot. */
int favcfg_read_active_slot(void);

/* Writes the mount slot (1..8) the user just actively selected via
 * [Source], saving immediately -- called right where cfg->active_index
 * itself changes, so a later crash/reset still remembers the choice
 * correctly. */
void favcfg_write_active_slot(int mount_slot);

#endif
