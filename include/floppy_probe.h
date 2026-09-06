#ifndef FLOPPY_PROBE_H
#define FLOPPY_PROBE_H

/* FLOPPY.PRG protocol layer: talks to the GEMDRVEMUL_FLOPPY_* commands
 * (SideTNFS-Firmware, romemul/include/commands.h -- see that repository's
 * floppyemu/main branch) added for this project. Cross-checked against
 * romemul/include/{commands.h,gemdrvemul.h,sidetnfs_floppy_emul.h} -- no
 * offsets/lengths/statuses guessed.
 *
 * Architecture note: subcommands 0x1D-0x22 (GET_CONFIG_INFO/GET_PROFILE/
 * SET_PROFILE/DELETE_PROFILE/SET_ACTIVE_PROFILE/SAVE_PROFILES, the
 * firmware's old flash-backed profile store) are GONE -- their command
 * codes are freed, not reused. Source management (CONFIG.CFG) is now
 * entirely local to FLOPPY.PRG (see favcfg.c); the Pico is a stateless
 * "browse/start this, given these connection params" service, addressed
 * via FloppySourceDescriptor below instead of a profile_index.
 *
 * This module is a pure protocol layer, mirroring SideTNFS-Config's own
 * sidetnfs_probe.c transport pattern (random-token handshake, string-field
 * endianness handling) verbatim: it knows nothing about the UI's
 * ProfileConfig/Profile model (profile.h). dialog.c translates explicitly
 * between the two, the same way SideTNFS-Config's dialog.c translates
 * between SideTnfsDriveInfo and Drive.
 *
 * Entirely independent of SideTNFS-Config's own sidetnfs_probe.c/.h --
 * different command IDs, different ROM3 offsets, different wire structs.
 * FLOPPY.PRG does not depend on that repository at build time. */

#define FLOPPY_PROBE_OK      0
#define FLOPPY_PROBE_TIMEOUT 1

#define FLOPPY_HOST_LEN      64
/* BROWSE_OPEN's start_directory field -- matches PROFILE_STARTDIR_LEN
 * (profile.h), the local UI-side field it's always built from. */
#define FLOPPY_STARTDIR_LEN  256

/* Matches sidetnfs_floppy_backend_t value-for-value. */
#define FLOPPY_BACKEND_TNFS 1
#define FLOPPY_BACKEND_SD   2

/* One source's connection parameters -- shared shape for BROWSE_OPEN and
 * SESSION_START, and field-for-field identical to one Favorites/Carousel
 * table entry's own backend/port/host (see favcfg.h's FavcfgTableEntry).
 * The Pico no longer stores any profile/source state of its own at all
 * (GEMDRVEMUL_FLOPPY_GET_CONFIG_INFO/GET_PROFILE/SET_PROFILE/
 * DELETE_PROFILE/SET_ACTIVE_PROFILE/SAVE_PROFILES and their whole
 * flash-backed profile store are gone from the firmware, per the
 * architecture change that introduced this struct) -- every command that
 * used to take a profile_index now takes this descriptor directly, and
 * FLOPPY.PRG's own local CONFIG.CFG (favcfg.c) is the sole place any of
 * this is remembered. */
typedef struct {
    unsigned long backend; /* FLOPPY_BACKEND_* */
    unsigned long port;    /* TNFS only, 0 for SD */
    char host[FLOPPY_HOST_LEN]; /* TNFS only, "" for SD */
} FloppySourceDescriptor;

/* Step 2 -- real LFN directory browser (GEMDRVEMUL_FLOPPY_BROWSE_*,
 * subcommands 0x23-0x26, SideTNFS-Firmware romemul/include/commands.h,
 * floppyemu branch). Entirely separate status space from FLOPPY_STATUS_*
 * above -- mirrors sidetnfs_floppy_browse_status_t
 * (romemul/include/sidetnfs_floppy_browse.h) value-for-value. This is a
 * pure protocol client, same layering as the rest of this file: it knows
 * nothing about the UI's file-list widgets. */
#define FLOPPY_BROWSE_OK                        0
#define FLOPPY_BROWSE_ERR_INVALID_PROFILE       1
#define FLOPPY_BROWSE_ERR_SOURCE_NOT_CONFIGURED 2
#define FLOPPY_BROWSE_ERR_TNFS_NOT_CONNECTED    3
#define FLOPPY_BROWSE_ERR_TNFS_HOST_UNREACHABLE 4
#define FLOPPY_BROWSE_ERR_SD_NOT_PRESENT        5
#define FLOPPY_BROWSE_ERR_DIR_NOT_FOUND         6
#define FLOPPY_BROWSE_ERR_ACCESS_DENIED         7
#define FLOPPY_BROWSE_ERR_PATH_TOO_LONG         8
#define FLOPPY_BROWSE_ERR_NAME_TOO_LONG         9
#define FLOPPY_BROWSE_ERR_INVALID_PAGE_REQUEST  10
#define FLOPPY_BROWSE_STATUS_END_OF_DIRECTORY   11 /* not a hard error -- a well-formed, valid-empty page */
#define FLOPPY_BROWSE_ERR_STALE_GENERATION      12
#define FLOPPY_BROWSE_ERR_BACKEND_ERROR         13
#define FLOPPY_BROWSE_ERR_NOT_OPEN              14
/* NOT an error: floppy_probe_browse_get_page() polls this internally and
 * never returns it to its own caller -- a TNFS walk that needs more than a
 * handful of round trips is resumed across several GET_PAGE requests
 * rather than blocking the firmware's dispatch loop for a long stretch
 * (see sidetnfs_floppy_browse.h). Listed here only so the status space
 * matches the firmware's enum value-for-value. */
#define FLOPPY_BROWSE_STATUS_IN_PROGRESS        15

#define FLOPPY_BROWSE_CWD_LEN      256 /* matches FLOPPY_BROWSE_CWD_LEN, sidetnfs_floppy_browse.h */
#define FLOPPY_BROWSE_NAME_LEN     256 /* matches FLOPPY_BROWSE_NAME_LEN, sidetnfs_floppy_browse.h */
#define FLOPPY_BROWSE_PAGE_ENTRIES 15  /* matches FLOPPY_BROWSE_PAGE_ENTRIES, sidetnfs_floppy_browse.h -- also FM_MAX_VISIBLE_FILES (dialog.c), one firmware page IS one screen now */

typedef struct {
    unsigned long status;     /* FLOPPY_BROWSE_* */
    unsigned long generation; /* echo this back on every subsequent CHANGE_DIR/GET_PAGE call */
    char cwd[FLOPPY_BROWSE_CWD_LEN];
} FloppyBrowseResult;

/* entries beyond `count` are zeroed by the firmware -- never assume
 * leftover content from an earlier page. Atari ST RAM is not the tight
 * resource the Pico's is (see profile.h's own note), so this struct keeps
 * the full 15x256 page in one plain array rather than trying to save
 * space.
 *
 * Step 3: GET_DIR_PAGE/GET_FILE_PAGE were combined into one GET_PAGE --
 * the firmware itself now returns one page of up to FLOPPY_BROWSE_PAGE_ENTRIES
 * entries, directories always sorted before files within the page, so the
 * Atari side no longer fetches two separate pages and stitches them
 * together (see dialog.c's old fm_load_entries() for the pre-Step-3
 * approach). is_dir[i] tells the two apart, index-matched with entries[i]. */
typedef struct {
    unsigned long status;     /* FLOPPY_BROWSE_* -- FLOPPY_BROWSE_STATUS_END_OF_DIRECTORY is not an error */
    unsigned long generation;
    unsigned long page_index;
    unsigned int count;    /* 0..FLOPPY_BROWSE_PAGE_ENTRIES */
    unsigned int has_prev;
    unsigned int has_next;
    char entries[FLOPPY_BROWSE_PAGE_ENTRIES][FLOPPY_BROWSE_NAME_LEN];
    int is_dir[FLOPPY_BROWSE_PAGE_ENTRIES]; /* 1 = directory, 0 = file */
} FloppyPageResult;

/* Every function below returns FLOPPY_PROBE_OK / FLOPPY_PROBE_TIMEOUT for
 * the communication result -- a FLOPPY_PROBE_OK communication result says
 * nothing by itself about whether the firmware accepted/could satisfy the
 * request; check the function's own out-parameter status too. */

/* Step 2/3 browser. out->status carries the real browse result and must
 * always be checked separately, including on FLOPPY_PROBE_OK.
 * start_directory is the Browser's chosen starting point under src's own
 * root (TNFS always mounts server root now -- see
 * PROFILE_STARTDIR_LEN's own comment in profile.h; SD's own root is
 * wherever the SD card is mounted). */
int floppy_probe_browse_open(const FloppySourceDescriptor *src, const char *start_directory, FloppyBrowseResult *out);
int floppy_probe_browse_change_dir(unsigned long generation, int go_up, const char *name, FloppyBrowseResult *out);
/* One combined page (dirs sorted before files, see FloppyPageResult's own
 * comment) -- replaces the old separate get_dir_page()/get_file_page(). */
int floppy_probe_browse_get_page(unsigned long generation, unsigned long page_index, FloppyPageResult *out);

/* ------------------------------------------------------------------
 * Phase 5 -- packed Favorites upload + session start
 * (GEMDRVEMUL_FLOPPY_FAVORITES_WRITE_CHUNK/_WRITE_CHECK/_COMMIT/
 * GEMDRVEMUL_FLOPPY_SESSION_START, subcommands 0x26/0x27/0x28/0x29).
 * Deliberately includes favcfg.h and uses FavcfgSession directly, rather
 * than duplicating an equivalent ~16KB wire struct the way
 * FloppyProfileInfo duplicates Profile above -- the packed table+strings
 * shape in favcfg.h WAS this wire format from the start (see its own
 * comment), so a second copy of the same layout would be pure
 * boilerplate, not a meaningful separation. Every other pairing above
 * keeps the UI/wire separation because the two shapes genuinely differ
 * (field widths, presence/absence of drive-specific fields); this one
 * doesn't. */
#include "favcfg.h"

/* Mirrors sidetnfs_floppy_emul_status_t (SideTNFS-Firmware,
 * romemul/include/sidetnfs_floppy_emul.h) value-for-value -- shared by
 * FLOPPY_FAVORITES_* and FLOPPY_SESSION_START's own status field, same
 * status space the firmware uses for both. */
#define FLOPPY_SESSION_OK                        0
#define FLOPPY_SESSION_ERR_INVALID_PROFILE       1
#define FLOPPY_SESSION_ERR_SOURCE_NOT_CONFIGURED 2
#define FLOPPY_SESSION_ERR_TNFS_NOT_CONNECTED    3
#define FLOPPY_SESSION_ERR_TNFS_HOST_UNREACHABLE 4
#define FLOPPY_SESSION_ERR_SD_NOT_PRESENT        5
#define FLOPPY_SESSION_ERR_FILE_NOT_FOUND        6
#define FLOPPY_SESSION_ERR_ACCESS_DENIED         7
#define FLOPPY_SESSION_ERR_PATH_TOO_LONG         8
#define FLOPPY_SESSION_ERR_FILESIZE_INVALID      9
#define FLOPPY_SESSION_ERR_BPB_INVALID           10
#define FLOPPY_SESSION_ERR_GEOMETRY_UNSUPPORTED  11
#define FLOPPY_SESSION_ERR_GEOMETRY_MISMATCH     12
#define FLOPPY_SESSION_ERR_READ_FAILED           13
#define FLOPPY_SESSION_ERR_OUT_OF_RANGE          14
#define FLOPPY_SESSION_ERR_BACKEND_ERROR         15
#define FLOPPY_SESSION_ERR_NOT_OPEN              16

#define FLOPPY_SESSION_IMAGE_PATH_MAX 512 /* matches SIDETNFS_FLOPPY_FAVORITE_PATH_MAX/GEMDRVEMUL_FLOPPY_SESSION_IMAGE_PATH */

typedef struct {
    unsigned long status; /* FLOPPY_SESSION_* -- only meaningful when floppy_probe_session_start() itself returns FLOPPY_PROBE_OK */
    unsigned int sides;
    unsigned int sectors_per_track;
    unsigned int tracks;
    unsigned int bytes_per_sector;
} FloppySessionResult;

/* Uploads the whole packed session in <=FLOPPY_FAVORITES_CHUNK_MAX-byte
 * chunks (WRITE_CHUNK -> WRITE_CHECK per chunk, matching the existing
 * GEMDRVEMUL_WRITE_BUFF_CALL/_CHECK checksum-then-commit pattern this
 * protocol already uses for GEMDOS file writes), then one COMMIT
 * publishing the table/count/active_index/strings_used. Returns
 * FLOPPY_PROBE_OK/_TIMEOUT for the transport result; *out_status is the
 * firmware's own status (0 = OK) for whichever step failed, valid only
 * when this itself returns FLOPPY_PROBE_OK. On any non-OK outcome
 * (either return value), the caller must NOT proceed to
 * floppy_probe_session_start() -- no assumption is made about how much
 * of the upload the firmware actually received. A session with count==0
 * is still uploaded/committed normally (an empty Favorites table is
 * valid, just not useful) -- callers needing at least one favorite must
 * check that themselves before calling this. */
#define FLOPPY_FAVORITES_CHUNK_MAX 2048
int floppy_probe_favorites_upload(const FavcfgSession *session, unsigned long *out_status);

/* src is the FIRST Carousel entry's own connection descriptor -- there is
 * no more session-wide "active slot" on the firmware side at all (removed
 * along with the whole flash profile store), so every SESSION_START call
 * is fully self-contained, same as every Favorites/Carousel table entry
 * already is. image_path is ignored by the firmware when
 * install_floppy == 0 (see GEMDRVEMUL_FLOPPY_SESSION_START's own comment,
 * commands.h -- INSTALL_FLOPPY=NO does not require or validate an image),
 * so an empty string is fine in that case (src is still required even
 * then, matching the firmware's own new self-contained-per-entry design).
 * Returns FLOPPY_PROBE_OK/_TIMEOUT for the transport result; out->status/
 * geometry are only meaningful when this itself returns FLOPPY_PROBE_OK.
 * This command does not reset the Atari -- the caller decides
 * whether/when to do that based on out->status. */
int floppy_probe_session_start(const FloppySourceDescriptor *src, const char *image_path,
                                 int install_gemdrive, int install_floppy,
                                 FloppySessionResult *out);

#endif
