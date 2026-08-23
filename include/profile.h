#ifndef PROFILE_H
#define PROFILE_H

/* FLOPPY.PRG's own UI-side model for a TNFS floppy-image server profile.
 * Entirely independent from SideTNFS's GEMDOS drive list -- no drive
 * letter, no SD support, no transport choice (TNFS/UDP only). Eight FIXED
 * slots (index == firmware slot index), never compacted/re-sorted, same
 * convention SideTNFS-Config's DriveConfig uses for its 8 drive slots. */

#define MAX_PROFILES 8

#define PROFILE_NICK_LEN  24 /* 23 chars + NUL, matches SIDETNFS_FLOPPY_NICKNAME_LEN */
#define PROFILE_HOST_LEN  64 /* 63 chars + NUL, matches SIDETNFS_FLOPPY_HOST_LEN */
#define PROFILE_MOUNT_LEN 32 /* 31 chars + NUL, matches SIDETNFS_FLOPPY_MOUNTPATH_LEN */
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

typedef struct {
    ProfileSlotState state;
    char nickname[PROFILE_NICK_LEN];
    char host[PROFILE_HOST_LEN];
    int  port;
    char mount_path[PROFILE_MOUNT_LEN];
    char last_directory[PROFILE_LASTDIR_LEN];
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
 * no sensible built-in default server address for a floppy source, so this
 * is genuinely empty rather than pre-populated. */
void profile_config_init_defaults(ProfileConfig *cfg);

int profile_config_configured_count(const ProfileConfig *cfg);

#endif
