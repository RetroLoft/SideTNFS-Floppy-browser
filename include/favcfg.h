#ifndef FAVCFG_H
#define FAVCFG_H

#include "profile.h"

/* FLOPPY.PRG's local persistence -- deliberately plain-text, fixed line
 * order, no keywords/headers/hashes/binary records. See src/favcfg.c's
 * own file header for the full rationale and on-disk layout; this header
 * only exposes the small set of operations dialog.c needs.
 *
 * Architecture note: CONFIG.CFG used to be only a secondary "last-seen
 * mount snapshot" (Browser Sources themselves lived in the Pico's own
 * flash, fetched via GET_CONFIG_INFO/GET_PROFILE and pushed back via
 * SET_PROFILE/SAVE_PROFILES). That flash-backed profile store is gone
 * from the firmware entirely now -- CONFIG.CFG (favcfg_load_config()/
 * favcfg_save_config() below) is the SOLE source of truth for Browser
 * Sources, read/written directly, with zero round-trips to the
 * cartridge for source management. Favorites (FAVORITS.CFG) also
 * changed shape as part of the same architecture change: each Favorite
 * is now fully self-contained (its own backend+host+port+full path)
 * rather than living inside one particular source's own file, so it
 * never goes stale when a Source is edited or removed. */

/* Reused for both display-name buffers (favcfg_read_page_favorite_names())
 * and the browse protocol's own FLOPPY_BROWSE_NAME_LEN -- full filenames
 * are always kept, never truncated in storage (only the on-screen display
 * may shorten them). */
#define FAVCFG_NAME_LEN 256

/* Must be called once, before any other favcfg_*() function -- captures
 * FLOPPY.PRG's own directory (the current directory at program launch,
 * per standard GEM/TOS convention) so FLOPPY.CFG is always found next to
 * the program, never wherever the user's current directory happens to
 * be for some unrelated reason. */
void favcfg_init(void);

/* ------------------------------------------------------------------
 * CONFIG.CFG -- Browser Sources (the 8 fixed profile slots), now the
 * sole local source of truth -- see this file's own header comment.
 * ------------------------------------------------------------------ */

/* Reads CONFIG.CFG directly into *cfg -- every slot's full state
 * (empty/disabled/enabled, backend, nickname, host/port/browser start
 * directory or SD path, last browsed directory), plus the last active
 * slot. Returns 1 if CONFIG.CFG existed and was read, 0 if missing (a
 * genuine first run, or an old pre-architecture-change install) -- *cfg
 * is left untouched on a 0 return; the caller falls back to
 * profile_config_init_defaults() itself, same convention the old
 * firmware-fetch path used. */
int favcfg_load_config(ProfileConfig *cfg);

/* Writes CONFIG.CFG directly from cfg's current 8 profiles, saving
 * immediately (streaming rewrite via temp file, same discipline as every
 * other favcfg_*() writer below). Shows a GEM alert only on a genuine
 * write failure. This is the ONLY thing [Save] in the Edit Sources
 * dialog does now -- no firmware round-trip of any kind any more. */
void favcfg_save_config(const ProfileConfig *cfg);

/* ------------------------------------------------------------------
 * ACTIVE.CFG -- last-selected source slot
 *
 * One bare line, the mount slot number (1..8) -- same "no keywords, no
 * header" plain-text style as every other file here. Deliberately
 * separate from CONFIG.CFG rather than a 9th line tacked onto it: that
 * file's own fixed layout is unrelated to this value, which has nothing
 * to do with any one slot's own definition. Exists because picking a
 * source via [Source] is deliberately an in-memory-only choice until an
 * explicit [Save] -- so without this, the choice never survived a
 * restart.
 * ------------------------------------------------------------------ */

/* Returns 0 (not 1..8) if the file is missing or its content is out of
 * range; the caller must still check the slot is actually configured
 * before trusting it, same as any other mount slot. */
int favcfg_read_active_slot(void);

/* Saving immediately -- called right where cfg->active_index itself
 * changes, so a later crash/reset still remembers the choice correctly. */
void favcfg_write_active_slot(int mount_slot);

/* ------------------------------------------------------------------
 * FAVORITS.CFG -- the global Favorites catalog (replaces the old
 * MOUNTn.CFG-per-source design, one file per mount slot). Each Favorite
 * is now fully self-contained -- its own backend+host+port+complete
 * path -- so it is global (NOT parameterized by mount_slot/source at
 * all any more) and never goes stale when a Browser Source is edited or
 * removed. When a Favorite is actually used (Start/Add to Carousel),
 * dialog.c validates its backend/server/path at that point; if
 * unavailable, it alerts and leaves the Favorite untouched -- no
 * orphan/relink management here, per this project's own explicit
 * "keep this simple" instruction.
 * ------------------------------------------------------------------ */

#define FAVCFG_FAVORITES_MAX_COUNT 60

/* Reuses profile.h's own PROFILE_HOST_LEN rather than a fresh constant --
 * same value, same field, no reason for these to ever drift apart. */
#define FAVCFG_HOST_LEN PROFILE_HOST_LEN

/* Matches the wire's own SESSION_START image-path budget
 * (floppy_probe.h's FLOPPY_SESSION_IMAGE_PATH_MAX) by convention, not a
 * shared #define -- favcfg.h deliberately stays independent of
 * floppy_probe.h (only floppy_probe.h includes favcfg.h, never the other
 * way around), same "numbers happen to match, layers stay separate"
 * precedent PROFILE_HOST_LEN/FLOPPY_HOST_LEN already set elsewhere in
 * this project. */
#define FAVCFG_PATH_LEN 512

/* One Favorite -- also the shape one Carousel entry needs (dialog.c's
 * own in-RAM Carousel array uses this same struct directly, see its own
 * comment there). backend == 0 marks an empty/unused slot; host/port are
 * only meaningful when backend == PROFILE_BACKEND_TNFS (both left blank/
 * 0 for SD, matching how the wire format leaves them present-but-unused
 * for an SD entry too). */
typedef struct {
    int backend;                /* 0 = empty, else PROFILE_BACKEND_TNFS/PROFILE_BACKEND_SD */
    char host[FAVCFG_HOST_LEN]; /* TNFS only */
    int port;                   /* TNFS only, 0 otherwise */
    char path[FAVCFG_PATH_LEN]; /* complete path from the TNFS server root, or complete SD path; "" when empty */
} FavoriteRecord;

/* Reads just the display name (the final path component, i.e. everything
 * after the last '/') for favorite 1..60 into out_name -- "" if out of
 * range, empty, or FAVORITS.CFG doesn't exist yet. */
void favcfg_read_favorite_name(int favorite, char *out_name, int out_name_cap);

/* Same batching idea as this file's other page-read functions: ONE file
 * open for `count` CONSECUTIVE favorites starting at `first_favorite`
 * (1..60), not one open per favorite -- built for one visible Favorites
 * page (dialog.c's FA_ROWS) at a time. */
void favcfg_read_page_favorite_names(int first_favorite, int count,
                                       char out_names[][FAVCFG_NAME_LEN]);

/* Reads the full record for favorite 1..60 -- used when a single
 * favorite is actually selected/started/moved. rec->backend is left 0
 * for an out-of-range favorite, an empty slot, or a missing file. */
void favcfg_read_favorite(int favorite, FavoriteRecord *rec);

/* Writes favorite `favorite` from *rec, saving immediately. A record
 * with backend == 0 clears the slot (same effect as
 * favcfg_erase_favorite()). */
void favcfg_write_favorite(int favorite, const FavoriteRecord *rec);

/* Clears favorite `favorite`, saving immediately. */
void favcfg_erase_favorite(int favorite);

/* Clears every favorite (all 60), saving immediately -- the user's own
 * "erase all" confirmation (Ctrl+Delete in Favorites). */
void favcfg_erase_all_favorites(void);

/* Swaps favorite `from` and favorite `to` in one streaming rewrite pass
 * (so if `to` was empty, `from` becomes empty too -- nothing is lost
 * either way), saving immediately. A no-op if from == to. */
void favcfg_move_favorite(int from, int to);

/* ------------------------------------------------------------------
 * Carousel/Favorites packed session upload (Start flow)
 * ------------------------------------------------------------------ */

/* Must match SideTNFS-Firmware's SIDETNFS_FLOPPY_FAVORITES_MAX_COUNT/
 * _STRINGS_MAX/_EMPTY_OFFSET (romemul/include/gemdrvemul.h) -- the packed
 * table+strings shape FLOPPY_FAVORITES_WRITE_CHUNK/_COMMIT expect. */
#define FAVCFG_SESSION_MAX_COUNT 60
#define FAVCFG_SESSION_STRINGS_MAX 16384UL
#define FAVCFG_SESSION_EMPTY_OFFSET 0xFFFFU

/* One Favorites/Carousel table entry -- field-for-field the same shape
 * SideTNFS-Firmware's own ROM3 table uses now (4 x 16-bit words: backend,
 * port, host_offset, path_offset; see this project's own architecture-
 * change task brief, which replaced the old path-offset-only table).
 * backend == 0 marks an empty/unused slot -- neither offset field is
 * meaningful then. host_offset is only meaningful when
 * backend == PROFILE_BACKEND_TNFS (send FAVCFG_SESSION_EMPTY_OFFSET for
 * an SD entry's host_offset for clarity, though the firmware does not
 * enforce that). */
typedef struct {
    unsigned int backend;     /* 0 = empty, else PROFILE_BACKEND_TNFS/PROFILE_BACKEND_SD */
    unsigned int port;        /* TNFS only, 0 otherwise */
    unsigned int host_offset; /* byte offset into strings[], TNFS only */
    unsigned int path_offset; /* byte offset into strings[] -- the complete path */
} FavcfgTableEntry;

/* out itself is large (~16.4KB, dominated by strings[]) -- callers MUST
 * use a static/global instance, never a stack local (TOS's own default
 * stack, see gemdrive_prg.s-style mystack reservations elsewhere in this
 * project, is nowhere near 16KB) and never pass it by value. */
typedef struct {
    unsigned int count;        /* non-empty entries, 0..FAVCFG_SESSION_MAX_COUNT */
    unsigned int active_index; /* 0-based, meaningful only if count > 0 */
    FavcfgTableEntry table[FAVCFG_SESSION_MAX_COUNT];
    unsigned long strings_used;
    char strings[FAVCFG_SESSION_STRINGS_MAX]; /* packed, NUL-terminated host/path strings, back to back */
} FavcfgSession;

#endif
