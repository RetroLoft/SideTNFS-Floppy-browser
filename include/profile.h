#ifndef PROFILE_H
#define PROFILE_H

/* FLOPPY.PRG's own UI-side model for a floppy-image source profile: a
 * TNFS network share OR a local SD-card folder, chosen explicitly per
 * profile via `backend`. Entirely independent from SideTNFS's GEMDOS
 * drive list -- no drive letter, no intermediate GEMDOS drive mapping for
 * SD either. Eight FIXED slots (index == firmware slot index), never
 * compacted/re-sorted, same convention SideTNFS-Config's DriveConfig uses
 * for its 8 drive slots.
 *
 * No union here (unlike the firmware's own sidetnfs_floppy_profile_config_t)
 * -- Atari ST RAM is not the tight resource the Pico's is (see this
 * project's RESEARCH-STEP0.md and the real Pico-side RAM regression this
 * project hit once already), so plain always-present fields are clearer
 * UI-side code for a negligible ~600 bytes/profile. */

#define MAX_PROFILES 8

#define PROFILE_NICK_LEN  24  /* 23 chars + NUL, matches SIDETNFS_FLOPPY_NICKNAME_LEN */
#define PROFILE_HOST_LEN  64  /* 63 chars + NUL, matches SIDETNFS_FLOPPY_HOST_LEN -- TNFS only */
#define PROFILE_MOUNT_LEN 32  /* 31 chars + NUL, matches SIDETNFS_FLOPPY_MOUNTPATH_LEN -- TNFS only */
#define PROFILE_SDPATH_LEN 256 /* matches SIDETNFS_FLOPPY_SDPATH_LEN -- SD only, full path */
#define PROFILE_LASTDIR_LEN 256 /* matches SIDETNFS_FLOPPY_LASTDIR_LEN -- application
                                  * state, not directly editable in the profile editor */

/* Matches sidetnfs_floppy_profile_state_t (sidetnfs_floppy_config.h)
 * value-for-value. EMPTY: no stored configuration, every other field
 * meaningless/zeroed. DISABLED: a fully valid, stored configuration,
 * deliberately not the active profile. ENABLED: a fully valid, stored
 * configuration that can be selected as active. DISABLED and ENABLED are
 * both "configured". */
typedef enum {
    PROFILE_SLOT_EMPTY    = 0,
    PROFILE_SLOT_DISABLED = 1,
    PROFILE_SLOT_ENABLED  = 2
} ProfileSlotState;

/* Matches sidetnfs_floppy_backend_t value-for-value. Explicit, never
 * inferred from an empty hostname or any other implicit signal -- chosen
 * once by the user in the profile editor's Source: TNFS/SD toggle. */
typedef enum {
    PROFILE_BACKEND_TNFS = 1,
    PROFILE_BACKEND_SD   = 2
} ProfileBackend;

typedef struct {
    ProfileSlotState state;
    ProfileBackend backend;      /* meaningless when state == EMPTY */
    char nickname[PROFILE_NICK_LEN];
    char last_directory[PROFILE_LASTDIR_LEN]; /* common to both backends */

    /* TNFS only -- meaningless/blank when backend == PROFILE_BACKEND_SD */
    char host[PROFILE_HOST_LEN];
    int  port;
    char mount_path[PROFILE_MOUNT_LEN];

    /* SD only -- meaningless/blank when backend == PROFILE_BACKEND_TNFS */
    char sd_path[PROFILE_SDPATH_LEN];
} Profile;

typedef struct {
    Profile profiles[MAX_PROFILES]; /* fixed slots 0..7, never compacted/sorted */
    int active_index;               /* 0..MAX_PROFILES-1 */
} ProfileConfig;

/* Single source of truth for the three-state semantics -- never compare
 * ->state directly against PROFILE_SLOT_* elsewhere. */
int profile_slot_is_empty(const Profile *p);
int profile_slot_is_configured(const Profile *p); /* DISABLED or ENABLED */
int profile_slot_is_enabled(const Profile *p);

/* Builds the in-memory starting point when no firmware is present yet:
 * every slot EMPTY, active_index 0. Unlike SideTNFS's drive list, there is
 * no sensible built-in default server address or SD path for a floppy
 * source, so this is genuinely empty rather than pre-populated. */
void profile_config_init_defaults(ProfileConfig *cfg);

int profile_config_configured_count(const ProfileConfig *cfg);

#endif
