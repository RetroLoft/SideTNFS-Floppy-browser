#ifndef FLOPPY_PROBE_H
#define FLOPPY_PROBE_H

/* FLOPPY.PRG protocol layer: talks to the GEMDRVEMUL_FLOPPY_* commands
 * (SideTNFS-Firmware, romemul/include/commands.h, subcommands 0x1D-0x22 --
 * see that repository's floppyemu branch) added for this project. Cross-
 * checked against romemul/include/{commands.h,gemdrvemul.h,
 * sidetnfs_floppy_config.h} -- no offsets/lengths/statuses guessed.
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

#define FLOPPY_NICKNAME_LEN  24
#define FLOPPY_HOST_LEN      64
#define FLOPPY_MOUNTPATH_LEN 32
#define FLOPPY_SDPATH_LEN    256
#define FLOPPY_LASTDIR_LEN   256

/* Matches sidetnfs_floppy_profile_state_t (sidetnfs_floppy_config.h)
 * value-for-value. */
#define FLOPPY_PROFILE_STATE_EMPTY    0
#define FLOPPY_PROFILE_STATE_DISABLED 1
#define FLOPPY_PROFILE_STATE_ENABLED  2

/* Matches sidetnfs_floppy_backend_t value-for-value. */
#define FLOPPY_BACKEND_TNFS 1
#define FLOPPY_BACKEND_SD   2

/* protocol/status codes -- sidetnfs_floppy_config_status_t (sidetnfs_floppy_config.h) */
#define FLOPPY_STATUS_OK                    0
#define FLOPPY_STATUS_INVALID_INDEX         1
#define FLOPPY_STATUS_EMPTY_SLOT            2
#define FLOPPY_STATUS_INVALID_NICKNAME      3
#define FLOPPY_STATUS_INVALID_HOST          4
#define FLOPPY_STATUS_INVALID_PORT          5
#define FLOPPY_STATUS_INVALID_PROFILE_STATE 6
#define FLOPPY_STATUS_FLASH_WRITE_FAILED    7
#define FLOPPY_STATUS_CRC_MISMATCH          8
#define FLOPPY_STATUS_UNSUPPORTED_VERSION   9
#define FLOPPY_STATUS_INVALID_BACKEND       10
#define FLOPPY_STATUS_INVALID_SD_PATH       11

/* Flash-format/protocol version reported by GET_CONFIG_INFO --
 * SIDETNFS_FLOPPY_CONFIG_FLASH_VERSION on the firmware side. Bumped from 1
 * to 2 when the SD backend was added (new `backend` field, TNFS/SD union).
 * A firmware still reporting 1 predates the SD backend entirely. */
#define FLOPPY_CONFIG_PROTOCOL_VERSION 2UL

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
/* NOT an error: floppy_probe_browse_get_dir_page()/_get_file_page() poll
 * this internally and never return it to their own caller -- a TNFS walk
 * that needs more than a handful of round trips is resumed across several
 * GET_*_PAGE requests rather than blocking the firmware's dispatch loop
 * for a long stretch (see sidetnfs_floppy_browse.h). Listed here only so
 * the status space matches the firmware's enum value-for-value. */
#define FLOPPY_BROWSE_STATUS_IN_PROGRESS        15

#define FLOPPY_BROWSE_CWD_LEN      256 /* matches FLOPPY_BROWSE_CWD_LEN, sidetnfs_floppy_browse.h */
#define FLOPPY_BROWSE_NAME_LEN     256 /* matches FLOPPY_BROWSE_NAME_LEN, sidetnfs_floppy_browse.h */
#define FLOPPY_BROWSE_PAGE_ENTRIES 25  /* matches FLOPPY_BROWSE_PAGE_ENTRIES, sidetnfs_floppy_browse.h */

typedef struct {
    unsigned long status;     /* FLOPPY_BROWSE_* */
    unsigned long generation; /* echo this back on every subsequent CHANGE_DIR/GET_*_PAGE call */
    char cwd[FLOPPY_BROWSE_CWD_LEN];
} FloppyBrowseResult;

/* entries beyond `count` are zeroed by the firmware -- never assume
 * leftover content from an earlier page. Atari ST RAM is not the tight
 * resource the Pico's is (see profile.h's own note), so this struct keeps
 * the full 25x256 page in one plain array rather than trying to save
 * space. */
typedef struct {
    unsigned long status;     /* FLOPPY_BROWSE_* -- FLOPPY_BROWSE_STATUS_END_OF_DIRECTORY is not an error */
    unsigned long generation;
    unsigned long page_index;
    unsigned int count;    /* 0..FLOPPY_BROWSE_PAGE_ENTRIES */
    unsigned int has_prev;
    unsigned int has_next;
    char entries[FLOPPY_BROWSE_PAGE_ENTRIES][FLOPPY_BROWSE_NAME_LEN];
} FloppyPageResult;

typedef struct {
    unsigned long protocol_version;
    unsigned long max_profiles;
    unsigned long profile_count;
    unsigned long active_profile_index;
    unsigned long status;
} FloppyConfigInfo;

/* Wire record shared by GET_PROFILE (full) and SET_PROFILE (request, minus
 * status). Field lengths match the firmware's
 * sidetnfs_floppy_profile_config_t exactly (see sidetnfs_floppy_config.h),
 * independent of the UI's profile.h lengths even though the numbers happen
 * to match today. Unlike the firmware's own on-flash/RAM record, this wire
 * struct keeps host/mount_path and sd_path as separate, always-present
 * fields rather than a union -- the ROM3 window and this struct are not
 * the tight resource the Pico's static RAM is, and a union-free wire
 * layout is far less error-prone to pack/unpack correctly (see this
 * project's own SideTNFS-Firmware gemdrvemul.c SET_PROFILE handler for the
 * union-aliasing bug that motivated keeping the wire layout union-free). */
typedef struct {
    unsigned long status; /* only meaningful after GET_PROFILE/SET_PROFILE etc return OK */
    unsigned long state;  /* FLOPPY_PROFILE_STATE_* -- EMPTY/DISABLED/ENABLED */
    unsigned long backend; /* FLOPPY_BACKEND_* -- meaningless when state == EMPTY */
    unsigned long port;   /* TNFS only */
    char nickname[FLOPPY_NICKNAME_LEN];
    char last_directory[FLOPPY_LASTDIR_LEN];
    char host[FLOPPY_HOST_LEN];             /* TNFS only */
    char mount_path[FLOPPY_MOUNTPATH_LEN];  /* TNFS only */
    char sd_path[FLOPPY_SDPATH_LEN];        /* SD only */
} FloppyProfileInfo;

/* Every function below returns FLOPPY_PROBE_OK / FLOPPY_PROBE_TIMEOUT for
 * the communication result. The firmware's own protocol status (OK /
 * INVALID_INDEX / ... ) is a separate result, returned via *info->status
 * (GET_CONFIG_INFO/GET_PROFILE) or *out_status (the write commands) -- a
 * FLOPPY_PROBE_OK communication result says nothing about whether the
 * firmware accepted the request. */

int floppy_probe_get_config_info(FloppyConfigInfo *info);
int floppy_probe_get_profile(unsigned long index, FloppyProfileInfo *info);
int floppy_probe_set_profile(unsigned long index, const FloppyProfileInfo *in, unsigned long *out_status);
int floppy_probe_delete_profile(unsigned long index, unsigned long *out_status);
int floppy_probe_set_active_profile(unsigned long index, unsigned long *out_status);
int floppy_probe_save_profiles(unsigned long *out_status);

/* Step 2 browser. All four return FLOPPY_PROBE_OK/FLOPPY_PROBE_TIMEOUT for
 * the communication result, same convention as every function above --
 * out->status carries the real browse result and must always be checked
 * separately, including on FLOPPY_PROBE_OK. */
int floppy_probe_browse_open(unsigned long profile_index, FloppyBrowseResult *out);
int floppy_probe_browse_change_dir(unsigned long generation, int go_up, const char *name, FloppyBrowseResult *out);
int floppy_probe_browse_get_dir_page(unsigned long generation, unsigned long page_index, FloppyPageResult *out);
int floppy_probe_browse_get_file_page(unsigned long generation, unsigned long page_index, FloppyPageResult *out);

#endif
