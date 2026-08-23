#include <string.h>
#include "profile.h"

int profile_slot_is_empty(const Profile *p)
{
    return p->state == PROFILE_SLOT_EMPTY;
}

int profile_slot_is_configured(const Profile *p)
{
    return p->state == PROFILE_SLOT_DISABLED || p->state == PROFILE_SLOT_ENABLED;
}

int profile_slot_is_enabled(const Profile *p)
{
    return p->state == PROFILE_SLOT_ENABLED;
}

void profile_config_init_defaults(ProfileConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    /* Every slot stays EMPTY (state == 0 from the memset above);
     * active_index stays 0 (meaningless until a profile is configured). */
}

int profile_config_configured_count(const ProfileConfig *cfg)
{
    int i, n = 0;
    for (i = 0; i < MAX_PROFILES; i++)
        if (profile_slot_is_configured(&cfg->profiles[i]))
            n++;
    return n;
}
