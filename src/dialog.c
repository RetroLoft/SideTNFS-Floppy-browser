/*
 * FLOPPY.PRG - GEM dialogs built in C, no .RSC file. Same convention as
 * SideTNFS-Config's SIDETNFS.PRG (see that project's dialog.c header
 * comment) -- deliberately followed here too, since this project reuses
 * that codebase's generic GEM plumbing.
 *
 * Target : Atari ST / Mega STE, TOS 1.x / 2.x, 68000 CPU.
 * Minimum: 640x200 medium resolution (see main.c's low-res guard).
 *
 * Conventions (same as SIDETNFS.PRG):
 *   - All declarations at top of each function (C89).
 *   - No dynamic memory.
 *   - All EXIT/TOUCHEXIT buttons use TOUCHEXIT for TOS 2.06 compatibility
 *     (TOS bug: plain EXIT buttons are not delivered while a text field is
 *     active; TOUCHEXIT fires on mouse-down before edit-mode routing).
 *   - After form_do() returns for a TOUCHEXIT button, poll graf_mkstate()
 *     until the mouse button is released before showing form_alert().
 *
 * Unlike SIDETNFS.PRG, this file factors the "run a modal dialog" and
 * "compute character-cell layout metrics" sequences (repeated ~10 times,
 * hand-copied, in that project -- see RESEARCH-STEP0.md section 3.6/3.4)
 * into small shared helpers (dialog_open/dialog_click/dialog_close,
 * layout_metrics_get) used by every dialog below. This is the one
 * concrete cleanup RESEARCH-STEP0.md recommended making from the start,
 * not a general-purpose framework -- everything else here stays as small
 * and direct as the source material.
 *
 * Explicitly NOT reused from SIDETNFS.PRG (see RESEARCH-STEP0.md section
 * 3.7): WiFi/network config, RTC/NTP config, GEMDOS drive letters, the
 * SideTNFS status overview. This project has no drive letters and no
 * SideTNFS GEMDOS drive concept at all.
 */

#include <gem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dialog.h"
#include "profile.h"
#include "floppy_probe.h"
#include "favcfg.h"

/* ================================================================== */
/* Shared generic helpers                                              */
/* Copied near-verbatim from SIDETNFS-Config's dialog.c -- proven, pure  */
/* GEM UI plumbing with no SideTNFS-specific meaning (RESEARCH-STEP0.md  */
/* section 3.8, reuse list A).                                          */
/* ================================================================== */

static void fill_n(char *s, char c, int n)
{
    int i;
    for (i = 0; i < n; i++) s[i] = c;
    s[n] = '\0';
}

/* Copy val into buf, NUL-terminate after actual content.
 * Do NOT space-pad: GEM places the edit cursor at strlen(te_ptext). */
static void set_buf(char *buf, int n, const char *val)
{
    int i;
    int vlen = (int)strlen(val);
    if (vlen > n - 1) vlen = n - 1;
    for (i = 0; i < vlen; i++) buf[i] = val[i];
    buf[vlen] = '\0';
}

static void init_ti(TEDINFO *ti, char *text, char *tmpl, char *vld, int bufsz)
{
    ti->te_ptext     = text;
    ti->te_ptmplt    = tmpl;
    ti->te_pvalid    = vld;
    ti->te_font      = IBM;
    ti->te_fontid    = 0;
    ti->te_just      = TE_LEFT;
    ti->te_color     = 0x1100;
    ti->te_fontsize  = 0;
    ti->te_thickness = -1;
    ti->te_txtlen    = (short)bufsz;
    ti->te_tmplen    = (short)bufsz;
}

/* Assign geometry and attributes for one object in a given tree. */
static void set_obj(OBJECT *tree, int id, int type, int flags, int state,
                    int x, int y, int w, int h)
{
    tree[id].ob_type   = (unsigned short)type;
    tree[id].ob_flags  = (unsigned short)flags;
    tree[id].ob_state  = (unsigned short)state;
    tree[id].ob_x      = (short)x;
    tree[id].ob_y      = (short)y;
    tree[id].ob_width  = (short)w;
    tree[id].ob_height = (short)h;
}

/* Wire a flat tree: objects 1..n-1 are all direct children of root. */
static void wire_tree(OBJECT *tree, int n)
{
    int i;
    tree[0].ob_next = -1;
    tree[0].ob_head = 1;
    tree[0].ob_tail = n - 1;
    for (i = 1; i < n - 1; i++) {
        tree[i].ob_next = i + 1;
        tree[i].ob_head = -1;
        tree[i].ob_tail = -1;
    }
    tree[n-1].ob_next  = 0;
    tree[n-1].ob_head  = -1;
    tree[n-1].ob_tail  = -1;
    tree[n-1].ob_flags |= LASTOB;
}

/* Return 1 if buf contains at least one non-space, non-NUL character. */
static int buf_nonempty(const char *buf)
{
    const char *p;
    for (p = buf; *p; p++)
        if (*p != ' ') return 1;
    return 0;
}

/* Copy buf into dest (up to destlen-1 chars), stripping trailing spaces. */
static void buf_copy(const char *buf, char *dest, int destlen)
{
    int n;
    strncpy(dest, buf, destlen - 1);
    dest[destlen - 1] = '\0';
    n = (int)strlen(dest);
    while (n > 0 && dest[n-1] == ' ') dest[--n] = '\0';
}

/* ================================================================== */
/* New shared helpers (RESEARCH-STEP0.md's recommended cleanup)        */
/* ================================================================== */

/* Character-cell layout metrics, computed once per dialog _init() --
 * factors out the block SIDETNFS-Config's dialog.c hand-copies into every
 * one of its ~10 dialog _init() functions (graf_handle() + the
 * work-area-height-based row-height shrink for short screens). */
typedef struct {
    short cw, ch;  /* character cell width/height */
    int   rh;      /* row height (possibly shrunk for short screens) */
    int   tm;      /* top/edge margin */
    int   pitch;   /* row pitch = rh + a small gap, for multi-row dialogs */
    int   short_screen; /* 1 on a medium-res-height (<350px) screen -- see fm_dialog_init()'s own use */
    int   sw, sh;  /* real screen work-area width/height, pixels (wind_get WF_WORKXYWH) -- see fm_dialog_init()'s
                    * short-screen layout: char-multiples of cw/ch are only an ESTIMATE of screen size (a system
                    * font's actual cw is not guaranteed to be 8px), so the main window's own full-width/fullscreen
                    * layout on a short screen measures directly against these instead of guessing from cw. */
} LayoutMetrics;

static void layout_metrics_get(LayoutMetrics *lm)
{
    short bw, bh;
    short sx, sy, sw, sh;

    graf_handle(&lm->cw, &lm->ch, &bw, &bh);
    wind_get(0, WF_WORKXYWH, &sx, &sy, &sw, &sh);
    if (sh <= 0 || sh > 800) sh = 200;
    if (sw <= 0 || sw > 1280) sw = 640;

    lm->short_screen = (sh < 350);
    lm->rh    = (sh >= 350) ? (int)lm->ch : ((lm->ch > 8) ? lm->ch / 2 : (int)lm->ch);
    lm->tm    = (lm->rh / 4 > 2) ? lm->rh / 4 : 2;
    lm->pitch = lm->rh + 3;
    lm->sw    = sw;
    lm->sh    = sh;

    (void)sx; (void)sy; (void)bw; (void)bh;
}

/* The "run a modal dialog" open/close halves of the 5-step sequence every
 * editor in SIDETNFS-Config's dialog.c hand-copies (form_center ->
 * form_dial(FMD_START) -> objc_draw ... form_do loop ... ->
 * form_dial(FMD_FINISH)). The event loop itself (which button does what)
 * stays in each caller, since it is genuinely different per dialog -- only
 * the open/close boilerplate and the per-click TOUCHEXIT handling are
 * truly identical everywhere, so only those are factored here. */
typedef struct { short x, y, w, h; } DialogGeometry;

/* flush_topleft: 0 (every existing caller) keeps the normal
 * form_center()-computed, screen-centered position. 1 (the main window
 * only, and only on a medium-res-height screen -- see dialog_run()) opens
 * flush against the top-left corner instead, at the tree's own already-
 * set ROOT width/height: on a screen just barely too short for the
 * dialog's content, centering wastes an equal margin above AND below that
 * pushes the bottom (the button row) off-screen, whereas flush-top uses
 * every available pixel from y=0 down. This does not change the dialog's
 * own computed height -- if that alone still exceeds the physical screen
 * height, the very bottom stays clipped either way; it only reclaims the
 * margin form_center() would otherwise have wasted. */
static void dialog_open(OBJECT *tree, int root_id, DialogGeometry *geo, int flush_topleft)
{
    if (flush_topleft) {
        geo->x = 0;
        geo->y = 0;
        geo->w = tree[root_id].ob_width;
        geo->h = tree[root_id].ob_height;
    } else {
        form_center(tree, &geo->x, &geo->y, &geo->w, &geo->h);
    }
    form_dial(FMD_START, geo->x, geo->y, geo->w, geo->h, geo->x, geo->y, geo->w, geo->h);
    objc_draw(tree, root_id, MAX_DEPTH, geo->x, geo->y, geo->w, geo->h);
}

static void dialog_close(DialogGeometry *geo)
{
    form_dial(FMD_FINISH, geo->x, geo->y, geo->w, geo->h, geo->x, geo->y, geo->w, geo->h);
}

/* Shared "full window" frame geometry: title line, a divider, one more
 * header line, another divider, row_count rows, a third divider, one
 * button row. Both the main browser (FM_*) and the Favorites dialog
 * (FA_*) call this with the SAME row_count so their two windows are
 * pixel-identical in size on every resolution -- including the
 * medium-res-height "fullscreen, no border" treatment (short_screen
 * below, meant to pair with dialog_open()'s own flush_topleft). FA is a
 * different SCREEN within the app, not a differently-sized window; before
 * this helper existed the two dialogs' geometry was hand-duplicated and
 * drifted apart. */
#define STD_FRAME_CHARS 78 /* character-cell width on any screen tall enough not to need the short-screen treatment below */

typedef struct {
    int DW, DH, xl, yt;
    int ydiv1, ysecond, ydiv2, yrow0, ydiv3, ybtn;
    int short_screen;
    long root_spec; /* ob_spec.index for the G_BOX root -- border removed on a short screen, see fm_dialog_init()'s original comment on 0x00031070L/0x00001070L */
} StdFrame;

static void std_frame_get(StdFrame *f, const LayoutMetrics *lm, int row_count)
{
    f->short_screen = lm->short_screen;
    f->DW = lm->short_screen ? lm->sw : (STD_FRAME_CHARS * lm->cw);
    f->xl = lm->short_screen ? 1 : lm->cw;
    f->yt = lm->short_screen ? 1 : lm->tm;
    f->ydiv1 = f->yt + lm->rh + 1;
    if (lm->short_screen) {
        f->ysecond = f->yt + lm->rh + 1;
        f->ydiv2   = f->ysecond + lm->rh + 1;
        f->yrow0   = f->ydiv2 + 2 + 3;
        f->ybtn    = f->yrow0 + row_count * lm->pitch + 2;
    } else {
        f->ysecond = f->ydiv1 + 5;
        f->ydiv2   = f->ysecond + lm->rh + 2;
        f->yrow0   = f->ydiv2 + 5;
        f->ybtn    = f->yrow0 + row_count * lm->pitch + 2 + 7;
    }
    f->ydiv3 = f->yrow0 + row_count * lm->pitch + 2;
    f->DH = lm->short_screen ? (f->ybtn + lm->rh + 1) : (f->ybtn + lm->rh + lm->tm + 3);
    f->root_spec = lm->short_screen ? 0x00001070L : 0x00031070L;
}

/* Minimal "please wait" notice, shown for the one-time startup probe
 * sequence in dialog_run(). Same pattern as SIDETNFS-Config's
 * pw_dialog_show()/pw_dialog_hide() -- not interactive, no form_do(). */
enum { PW_ROOT = 0, PW_LINE, PW_NOBJS };
static OBJECT pw_dlg[PW_NOBJS];
static DialogGeometry pw_geo;

static void pw_dialog_show(void)
{
    LayoutMetrics lm;
    int DW, DH;
    const char *msg = "Please wait, loading configuration...";

    layout_metrics_get(&lm);

    DW = (int)(strlen(msg) + 4) * lm.cw;
    DH = (int)lm.ch + 2 * lm.tm;

    set_obj(pw_dlg, PW_ROOT, G_BOX, NONE, NORMAL, 0, 0, DW, DH);
    pw_dlg[PW_ROOT].ob_spec.index = 0x00031070L;

    set_obj(pw_dlg, PW_LINE, G_STRING, NONE, NORMAL, 2*lm.cw, lm.tm, DW - 4*lm.cw, (int)lm.ch);
    pw_dlg[PW_LINE].ob_spec.free_string = (char *)msg;

    wire_tree(pw_dlg, PW_NOBJS);
    dialog_open(pw_dlg, PW_ROOT, &pw_geo, 0);
}

static void pw_dialog_hide(void)
{
    dialog_close(&pw_geo);
}

/* ================================================================== */
/* Firmware status text                                                */
/* ================================================================== */

static const char *floppy_status_text(unsigned long status)
{
    switch (status) {
    case FLOPPY_STATUS_OK:                    return "OK.";
    case FLOPPY_STATUS_INVALID_INDEX:         return "Invalid slot index.";
    case FLOPPY_STATUS_EMPTY_SLOT:            return "Slot is empty.";
    case FLOPPY_STATUS_INVALID_NICKNAME:      return "Nickname is empty.";
    case FLOPPY_STATUS_INVALID_HOST:          return "Host is empty.";
    case FLOPPY_STATUS_INVALID_PORT:          return "Invalid port.";
    case FLOPPY_STATUS_INVALID_PROFILE_STATE: return "Invalid profile state.";
    case FLOPPY_STATUS_FLASH_WRITE_FAILED:    return "Flash write failed.";
    case FLOPPY_STATUS_CRC_MISMATCH:          return "Flash CRC mismatch.";
    case FLOPPY_STATUS_UNSUPPORTED_VERSION:   return "Unsupported protocol version.";
    case FLOPPY_STATUS_INVALID_BACKEND:       return "Invalid backend (not TNFS or SD).";
    case FLOPPY_STATUS_INVALID_SD_PATH:       return "SD path is empty.";
    default:                                  return "Unknown status.";
    }
}

/* ================================================================== */
/* Wire <-> UI profile translation                                     */
/* Explicit field-by-field translation, never memcpy() between the wire */
/* struct and the UI struct -- same discipline SIDETNFS-Config's         */
/* wire_to_ui_drive()/ui_to_wire_drive() use, for the same reason (their */
/* padding/alignment/field order are not proven identical).             */
/* ================================================================== */

static void wire_to_ui_profile(const FloppyProfileInfo *w, Profile *p)
{
    memset(p, 0, sizeof(*p));

    switch (w->state) {
    case FLOPPY_PROFILE_STATE_DISABLED: p->state = PROFILE_SLOT_DISABLED; break;
    case FLOPPY_PROFILE_STATE_ENABLED:  p->state = PROFILE_SLOT_ENABLED;  break;
    case FLOPPY_PROFILE_STATE_EMPTY:
    default:                            p->state = PROFILE_SLOT_EMPTY;   break;
    }
    if (p->state == PROFILE_SLOT_EMPTY)
        return; /* every other field stays zeroed -- meaningless when EMPTY */

    /* An unrecognized wire backend value defaults to TNFS -- matches the
     * firmware's own SET_PROFILE unpacking default (gemdrvemul.c) and can
     * only happen against a corrupted/mismatched-protocol firmware, since
     * sidetnfs_floppy_config_set_profile() itself never stores a value
     * outside TNFS/SD. */
    p->backend = (w->backend == FLOPPY_BACKEND_SD) ? PROFILE_BACKEND_SD : PROFILE_BACKEND_TNFS;

    p->port = (int)w->port;
    strncpy(p->nickname, w->nickname, PROFILE_NICK_LEN - 1);
    p->nickname[PROFILE_NICK_LEN - 1] = '\0';
    strncpy(p->last_directory, w->last_directory, PROFILE_LASTDIR_LEN - 1);
    p->last_directory[PROFILE_LASTDIR_LEN - 1] = '\0';
    strncpy(p->host, w->host, PROFILE_HOST_LEN - 1);
    p->host[PROFILE_HOST_LEN - 1] = '\0';
    strncpy(p->mount_path, w->mount_path, PROFILE_MOUNT_LEN - 1);
    p->mount_path[PROFILE_MOUNT_LEN - 1] = '\0';
    strncpy(p->sd_path, w->sd_path, PROFILE_SDPATH_LEN - 1);
    p->sd_path[PROFILE_SDPATH_LEN - 1] = '\0';
}

static void ui_to_wire_profile(const Profile *p, FloppyProfileInfo *w)
{
    memset(w, 0, sizeof(*w));

    switch (p->state) {
    case PROFILE_SLOT_DISABLED: w->state = FLOPPY_PROFILE_STATE_DISABLED; break;
    case PROFILE_SLOT_ENABLED:  w->state = FLOPPY_PROFILE_STATE_ENABLED;  break;
    case PROFILE_SLOT_EMPTY:
    default:                    w->state = FLOPPY_PROFILE_STATE_EMPTY;   break;
    }
    if (p->state == PROFILE_SLOT_EMPTY)
        return; /* every other field stays zeroed (memset above) */

    w->backend = (p->backend == PROFILE_BACKEND_SD) ? FLOPPY_BACKEND_SD : FLOPPY_BACKEND_TNFS;
    w->port = (unsigned long)p->port;
    strncpy(w->nickname, p->nickname, FLOPPY_NICKNAME_LEN - 1);
    strncpy(w->last_directory, p->last_directory, FLOPPY_LASTDIR_LEN - 1);
    strncpy(w->host, p->host, FLOPPY_HOST_LEN - 1);
    strncpy(w->mount_path, p->mount_path, FLOPPY_MOUNTPATH_LEN - 1);
    strncpy(w->sd_path, p->sd_path, FLOPPY_SDPATH_LEN - 1);
    /* NUL-termination of every field is already guaranteed by the
     * memset(w,0,...) above, same reasoning ui_to_wire_drive() gives.
     * Sending both TNFS and SD fields regardless of `backend` is
     * harmless -- the firmware only ever validates/stores whichever one
     * `backend` actually selects (see sidetnfs_floppy_config.c). */
}

/* ================================================================== */
/* Firmware load / save                                                */
/* ================================================================== */

static int validate_profile(const Profile *p, char *msg)
{
    if (!buf_nonempty(p->nickname)) {
        sprintf(msg, "[3][Validation error|Nickname is empty.][OK]");
        return 0;
    }
    if (p->backend == PROFILE_BACKEND_SD) {
        if (!buf_nonempty(p->sd_path)) {
            sprintf(msg, "[3][Validation error|SD path is empty.][OK]");
            return 0;
        }
    } else {
        if (!buf_nonempty(p->host)) {
            sprintf(msg, "[3][Validation error|Host is empty.][OK]");
            return 0;
        }
        if (p->port < 1 || p->port > 65535) {
            sprintf(msg, "[3][Validation error|Port must be 1-65535.][OK]");
            return 0;
        }
    }
    return 1;
}

/* Builds *out entirely from firmware: GET_CONFIG_INFO, then GET_PROFILE
 * for all eight fixed slots, always -- EMPTY is a normal state, never a
 * reason to skip a slot. Returns 1 on a fully consistent read, 0 on any
 * timeout/unexpected status/protocol version -- *out is left untouched on
 * failure. No alerts: this is the silent building block for the startup
 * load, same role SIDETNFS-Config's fetch_drive_config_from_firmware()
 * plays there. */
static int fetch_profile_config_from_firmware(ProfileConfig *out)
{
    FloppyConfigInfo info;
    FloppyProfileInfo wire;
    ProfileConfig built;
    int i;

    if (floppy_probe_get_config_info(&info) != FLOPPY_PROBE_OK)
        return 0;
    if (info.status != FLOPPY_STATUS_OK)
        return 0;
    if (info.protocol_version != FLOPPY_CONFIG_PROTOCOL_VERSION)
        return 0;
    if (info.max_profiles != (unsigned long)MAX_PROFILES)
        return 0;

    memset(&built, 0, sizeof(built));
    built.active_index = (int)info.active_profile_index;
    if (built.active_index < 0 || built.active_index >= MAX_PROFILES)
        built.active_index = 0;

    for (i = 0; i < MAX_PROFILES; i++) {
        if (floppy_probe_get_profile((unsigned long)i, &wire) != FLOPPY_PROBE_OK)
            return 0;
        if (wire.status != FLOPPY_STATUS_OK)
            return 0; /* only an out-of-range index is ever non-OK -- never sent here */
        wire_to_ui_profile(&wire, &built.profiles[i]);
    }

    *out = built;
    return 1;
}

/* No local config file -- same policy SIDETNFS.PRG documents (see this
 * project's README.md). If no firmware is found (or its configuration is
 * unusable), fall back to an in-memory default of eight empty slots. */
static int startup_load(ProfileConfig *cfg)
{
    if (fetch_profile_config_from_firmware(cfg))
        return 1;
    profile_config_init_defaults(cfg);
    return 0;
}

/* Pushes the full profile list to the firmware (RAM only: SET_PROFILE per
 * slot, then SET_ACTIVE_PROFILE if the active slot is actually configured
 * -- an EMPTY active slot is skipped rather than sent, since the firmware
 * rejects activating an empty slot), then commits it to flash with one
 * SAVE_PROFILES. This is the ONLY path in this application that ever
 * writes flash -- selecting a profile in the server selector, or editing
 * one in the profile editor, only ever changes the in-memory cfg until
 * this function actually runs (see RESEARCH-STEP0.md section 6 / this
 * project's own sidetnfs_floppy_config.c file header for the full
 * persistence-policy rationale: avoids frequent flash writes). */
static int perform_save(ProfileConfig *cfg, char *msg)
{
    FloppyProfileInfo wire;
    unsigned long status;
    int i;

    for (i = 0; i < MAX_PROFILES; i++) {
        ui_to_wire_profile(&cfg->profiles[i], &wire);
        if (floppy_probe_set_profile((unsigned long)i, &wire, &status) != FLOPPY_PROBE_OK) {
            sprintf(msg, "[3][Save failed|Profile %d: firmware not|responding (timeout).][OK]", i + 1);
            return 0;
        }
        if (status != FLOPPY_STATUS_OK) {
            sprintf(msg, "[3][Save failed|Profile %d: %s][OK]", i + 1, floppy_status_text(status));
            return 0;
        }
    }

    if (cfg->active_index >= 0 && cfg->active_index < MAX_PROFILES
        && profile_slot_is_configured(&cfg->profiles[cfg->active_index])) {
        if (floppy_probe_set_active_profile((unsigned long)cfg->active_index, &status) != FLOPPY_PROBE_OK) {
            sprintf(msg, "[3][Save failed|Active profile: firmware not|responding (timeout).][OK]");
            return 0;
        }
        if (status != FLOPPY_STATUS_OK) {
            sprintf(msg, "[3][Save failed|Active profile: %s][OK]", floppy_status_text(status));
            return 0;
        }
    }

    if (floppy_probe_save_profiles(&status) != FLOPPY_PROBE_OK) {
        sprintf(msg, "[3][Save failed|SAVE_PROFILES: firmware not|responding (timeout).][OK]");
        return 0;
    }
    if (status != FLOPPY_STATUS_OK) {
        sprintf(msg, "[3][Save failed|%s][OK]", floppy_status_text(status));
        return 0;
    }

    sprintf(msg, "[1][Profiles saved.][OK]");
    return 1;
}

/* ================================================================== */
/* Profile editor (FP_*)                                               */
/* Inspired by SIDETNFS-Config's TNFS-drive editor (TE_*), minus the     */
/* Drive/Transport fields -- a floppy source has no drive letter, and    */
/* TCP is not offered here either (same "UDP only, this phase" limit the */
/* reused firmware convention already applies). last_directory is        */
/* application state, not shown here (see profile.h's own comment).      */
/*                                                                        */
/* Unlike any editor in SIDETNFS-Config (which picks TNFS-vs-SD once, at  */
/* creation time, via add_disk_type_run(), by opening one of two          */
/* permanently-different dialogs), this editor is ONE dialog with a live  */
/* Source: TNFS/SD toggle -- the TNFS fields (Host/Port/Mount dir) and    */
/* the SD field (SD path) occupy the SAME reserved vertical span and are  */
/* shown/hidden via the OBJECT HIDETREE flag as the toggle is clicked, so */
/* the dialog's own size never changes. This is new UI plumbing this      */
/* project needed that the reused source material doesn't demonstrate.   */
/* ================================================================== */
enum {
    FP_ROOT = 0,
    FP_TITLE,
    FP_DIV1,
    FP_LBL_SOURCE, FP_SOURCE_TNFS_BTN, FP_SOURCE_SD_BTN,
    FP_LBL_NICK,  FP_NICK_EDIT,
    FP_LBL_HOST,  FP_HOST_EDIT,       /* TNFS only */
    FP_LBL_PORT,  FP_PORT_EDIT,       /* TNFS only */
    FP_LBL_MOUNT, FP_MOUNT_EDIT, FP_MOUNT_HINT, /* TNFS only */
    FP_LBL_SDPATH, FP_SDPATH_EDIT,    /* SD only */
    FP_LBL_ACTIVE, FP_ACTIVE_BTN,
    FP_DIV2,
    FP_DELETE, FP_OK, FP_CANCEL,
    FP_NOBJS
};
static OBJECT fp_dlg[FP_NOBJS];

/* Every TNFS-only vs SD-only object id, for the HIDETREE toggle below. */
static const int fp_tnfs_objs[] = { FP_LBL_HOST, FP_HOST_EDIT, FP_LBL_PORT, FP_PORT_EDIT, FP_LBL_MOUNT, FP_MOUNT_EDIT, FP_MOUNT_HINT };
static const int fp_sd_objs[]   = { FP_LBL_SDPATH, FP_SDPATH_EDIT };
#define FP_TNFS_OBJS_COUNT ((int)(sizeof(fp_tnfs_objs) / sizeof(fp_tnfs_objs[0])))
#define FP_SD_OBJS_COUNT   ((int)(sizeof(fp_sd_objs) / sizeof(fp_sd_objs[0])))

/* On-screen (x,y,w,h) for each fp_tnfs_objs[]/fp_sd_objs[] entry, snapshot
 * once by fp_dialog_init() right after it lays them out. The TNFS block
 * and the SD block deliberately occupy the SAME reserved vertical span
 * (see fp_dialog_init()'s own comment) so the dialog's height never
 * changes when Source is toggled -- but that means the CURRENTLY HIDDEN
 * block's editable objects (FP_HOST_EDIT/FP_MOUNT_EDIT/FP_SDPATH_EDIT)
 * still sit at the exact same coordinates as the visible ones, HIDETREE
 * or not. That overlap of two EDITABLE objects turned out to be real
 * hardware fragile: clicking into a visible text field could hit-test
 * against a HIDETREE'd-but-still-same-rectangle one instead and crash
 * (reproduced on real hardware, three bombs). fp_apply_backend_visibility()
 * below now also shrinks hidden objects to zero size (so they can never
 * be hit-tested at any click coordinate) and restores this snapshot when
 * showing them again, instead of relying on HIDETREE alone. */
static short fp_tnfs_geom[FP_TNFS_OBJS_COUNT][4]; /* x, y, w, h */
static short fp_sd_geom[FP_SD_OBJS_COUNT][4];

#define FP_BUF_NICK   PROFILE_NICK_LEN   /* 24 */
#define FP_BUF_HOST   PROFILE_HOST_LEN   /* 64 */
#define FP_BUF_PORT   7
#define FP_BUF_MOUNT  PROFILE_MOUNT_LEN  /* 32 */
/* NOT PROFILE_SDPATH_LEN (256) -- a 256-byte scrolling G_FBOXTEXT field
 * reproducibly crashed real hardware (three bombs, every time) the moment
 * it was clicked into, even after the HIDETREE-overlap fix above. 64
 * matches SIDETNFS-Config's own sd_path editor field exactly
 * (SideTNFS-Config/src/dialog.c TE_BUF_SDPATH) -- the only OTHER place in
 * this whole codebase that edits an SD path, and the only size actually
 * proven to work on real hardware. The firmware/wire sd_path field itself
 * stays 256 bytes (profile.h, sidetnfs_floppy_config.h) -- only this UI
 * editor's own buffer/TEDINFO is capped, so a folder path longer than 63
 * characters (already generous for a filesystem path) is truncated on
 * save via this editor. buf_copy()'s strncpy-based copy into the real
 * 256-byte Profile.sd_path field is unaffected either way. */
#define FP_BUF_SDPATH 64

static char buf_fp_nick  [FP_BUF_NICK];
static char buf_fp_host  [FP_BUF_HOST];
static char buf_fp_port  [FP_BUF_PORT];
static char buf_fp_mount [FP_BUF_MOUNT];
static char buf_fp_sdpath[FP_BUF_SDPATH];

static char tmpl_fp_nick[FP_BUF_NICK],     vld_fp_nick[FP_BUF_NICK];
static char tmpl_fp_host[FP_BUF_HOST],     vld_fp_host[FP_BUF_HOST];
static char tmpl_fp_port[FP_BUF_PORT],     vld_fp_port[FP_BUF_PORT];
static char tmpl_fp_mount[FP_BUF_MOUNT],   vld_fp_mount[FP_BUF_MOUNT];
static char tmpl_fp_sdpath[FP_BUF_SDPATH], vld_fp_sdpath[FP_BUF_SDPATH];

static TEDINFO ti_fp_nick, ti_fp_host, ti_fp_port, ti_fp_mount, ti_fp_sdpath;

/* Active/Inactive toggle -- single button, own text doubles as the value
 * display, same "button with changing text" idiom SIDETNFS-Config's
 * update_drive_active_button_text() uses. */
#define FP_ACTIVE_BUF 11
static char buf_fp_active[FP_ACTIVE_BUF];
static int fp_editor_enabled; /* 1 = Active/ENABLED, 0 = Inactive/DISABLED -- live edit state */

/* Source: TNFS / SD card -- two side-by-side buttons, exactly one
 * SELECTED at a time, manually kept mutually exclusive -- same "manual
 * SELECTED-pair radio idiom" SIDETNFS-Config's NC_DHCP_BTN/NC_STATIC_BTN
 * use (RESEARCH-STEP0.md section 3.8), not GEM's own RBUTTON grouping
 * (that codebase never uses RBUTTON either, per that same research). */
static ProfileBackend fp_editor_backend; /* live edit state */

static int fields_ready = 0;

static void shared_fields_init(void)
{
    if (fields_ready) return;
    fields_ready = 1;

    fill_n(tmpl_fp_nick,   '_', FP_BUF_NICK   - 1); fill_n(vld_fp_nick,   'X', FP_BUF_NICK   - 1);
    fill_n(tmpl_fp_host,   '_', FP_BUF_HOST   - 1); fill_n(vld_fp_host,   'X', FP_BUF_HOST   - 1);
    fill_n(tmpl_fp_port,   '_', FP_BUF_PORT   - 1); fill_n(vld_fp_port,   '9', FP_BUF_PORT   - 1);
    fill_n(tmpl_fp_mount,  '_', FP_BUF_MOUNT  - 1); fill_n(vld_fp_mount,  'X', FP_BUF_MOUNT  - 1);
    fill_n(tmpl_fp_sdpath, '_', FP_BUF_SDPATH - 1); fill_n(vld_fp_sdpath, 'X', FP_BUF_SDPATH - 1);

    init_ti(&ti_fp_nick,   buf_fp_nick,   tmpl_fp_nick,   vld_fp_nick,   FP_BUF_NICK);
    init_ti(&ti_fp_host,   buf_fp_host,   tmpl_fp_host,   vld_fp_host,   FP_BUF_HOST);
    init_ti(&ti_fp_port,   buf_fp_port,   tmpl_fp_port,   vld_fp_port,   FP_BUF_PORT);
    init_ti(&ti_fp_mount,  buf_fp_mount,  tmpl_fp_mount,  vld_fp_mount,  FP_BUF_MOUNT);
    init_ti(&ti_fp_sdpath, buf_fp_sdpath, tmpl_fp_sdpath, vld_fp_sdpath, FP_BUF_SDPATH);
}

static void update_fp_active_button_text(void)
{
    strncpy(buf_fp_active, fp_editor_enabled ? " Active   " : " Inactive ", FP_ACTIVE_BUF - 1);
    buf_fp_active[FP_ACTIVE_BUF - 1] = '\0';
}

static void update_fp_source_buttons(void)
{
    if (fp_editor_backend == PROFILE_BACKEND_SD) {
        fp_dlg[FP_SOURCE_SD_BTN].ob_state |= (unsigned short)SELECTED;
        fp_dlg[FP_SOURCE_TNFS_BTN].ob_state &= (unsigned short)(~SELECTED);
    } else {
        fp_dlg[FP_SOURCE_TNFS_BTN].ob_state |= (unsigned short)SELECTED;
        fp_dlg[FP_SOURCE_SD_BTN].ob_state &= (unsigned short)(~SELECTED);
    }
}

/* Shows one block's objects at their real, laid-out geometry (restored
 * from fp_tnfs_geom/fp_sd_geom) with HIDETREE cleared; hides the other
 * block by shrinking each of its objects to zero size AND setting
 * HIDETREE -- see fp_tnfs_geom's own comment for why zero size, not just
 * HIDETREE, is needed. Caller redraws FP_ROOT afterward. */
static void fp_set_block_visible(const int *ids, short geom[][4], int count, int visible)
{
    int i;
    for (i = 0; i < count; i++) {
        OBJECT *ob = &fp_dlg[ids[i]];
        if (visible) {
            ob->ob_x = geom[i][0];
            ob->ob_y = geom[i][1];
            ob->ob_width  = geom[i][2];
            ob->ob_height = geom[i][3];
            ob->ob_flags &= (unsigned short)(~HIDETREE);
        } else {
            ob->ob_x = ob->ob_y = ob->ob_width = ob->ob_height = 0;
            ob->ob_flags |= (unsigned short)HIDETREE;
        }
    }
}

static void fp_apply_backend_visibility(void)
{
    int show_tnfs = (fp_editor_backend != PROFILE_BACKEND_SD);

    fp_set_block_visible(fp_tnfs_objs, fp_tnfs_geom, FP_TNFS_OBJS_COUNT, show_tnfs);
    fp_set_block_visible(fp_sd_objs,   fp_sd_geom,   FP_SD_OBJS_COUNT,   !show_tnfs);
}

static void fp_dialog_init(int show_delete)
{
    LayoutMetrics lm;
    int DW, DH, xl, xf;
    int yt, ydiv1, ysource, ynick, yblock0, yhost, yport, ymount, ymounthint, ysdpath, yactive, ydiv2, ybtn;

    layout_metrics_get(&lm);

    DW = 46 * lm.cw;
    xl = 2 * lm.cw;
    xf = 13 * lm.cw;

    yt         = lm.tm;
    ydiv1      = yt + lm.rh + 1;
    ysource    = ydiv1 + 5;
    ynick      = ysource + lm.pitch;
    yblock0    = ynick + lm.pitch;
    /* TNFS block: 4 rows (host/port/mount/hint), reserved regardless of
     * which source is currently shown, so the dialog's own height never
     * changes when the user toggles Source. */
    yhost      = yblock0;
    yport      = yhost + lm.pitch;
    ymount     = yport + lm.pitch;
    ymounthint = ymount + lm.pitch;
    /* SD block: 1 row, sharing the same starting y as the TNFS block. */
    ysdpath    = yblock0;
    yactive    = yblock0 + 4 * lm.pitch;
    ydiv2      = yactive + lm.rh + 2;
    ybtn       = ydiv2 + 7;
    DH         = ybtn + lm.rh + lm.tm + 3;

    set_obj(fp_dlg, FP_ROOT, G_BOX, NONE, NORMAL, 0, 0, DW, DH);
    fp_dlg[FP_ROOT].ob_spec.index = 0x00031070L;

    set_obj(fp_dlg, FP_TITLE, G_STRING, NONE, NORMAL, 15*lm.cw, yt, 16*lm.cw, lm.rh);
    fp_dlg[FP_TITLE].ob_spec.free_string = "Floppy Source";

    set_obj(fp_dlg, FP_DIV1, G_BOX, NONE, NORMAL, lm.cw, ydiv1, DW - 2*lm.cw, 2);
    fp_dlg[FP_DIV1].ob_spec.index = 0x00001171L;

    set_obj(fp_dlg, FP_LBL_SOURCE, G_STRING, NONE, NORMAL, xl, ysource, 11*lm.cw, lm.rh);
    fp_dlg[FP_LBL_SOURCE].ob_spec.free_string = "Source:";
    set_obj(fp_dlg, FP_SOURCE_TNFS_BTN, G_BUTTON, EXIT | TOUCHEXIT, NORMAL, xf, ysource, 10*lm.cw, lm.rh);
    fp_dlg[FP_SOURCE_TNFS_BTN].ob_spec.free_string = "  TNFS  ";
    set_obj(fp_dlg, FP_SOURCE_SD_BTN, G_BUTTON, EXIT | TOUCHEXIT, NORMAL, xf + 11*lm.cw, ysource, 10*lm.cw, lm.rh);
    fp_dlg[FP_SOURCE_SD_BTN].ob_spec.free_string = "SD card ";

    set_obj(fp_dlg, FP_LBL_NICK, G_STRING, NONE, NORMAL, xl, ynick, 11*lm.cw, lm.rh);
    fp_dlg[FP_LBL_NICK].ob_spec.free_string = "Nickname:";
    set_obj(fp_dlg, FP_NICK_EDIT, G_FBOXTEXT, EDITABLE, NORMAL, xf, ynick, 23*lm.cw, lm.rh);
    fp_dlg[FP_NICK_EDIT].ob_spec.tedinfo = &ti_fp_nick;

    set_obj(fp_dlg, FP_LBL_HOST, G_STRING, NONE, NORMAL, xl, yhost, 11*lm.cw, lm.rh);
    fp_dlg[FP_LBL_HOST].ob_spec.free_string = "Host:";
    set_obj(fp_dlg, FP_HOST_EDIT, G_FBOXTEXT, EDITABLE, NORMAL, xf, yhost, 31*lm.cw, lm.rh);
    fp_dlg[FP_HOST_EDIT].ob_spec.tedinfo = &ti_fp_host;

    set_obj(fp_dlg, FP_LBL_PORT, G_STRING, NONE, NORMAL, xl, yport, 11*lm.cw, lm.rh);
    fp_dlg[FP_LBL_PORT].ob_spec.free_string = "Port:";
    set_obj(fp_dlg, FP_PORT_EDIT, G_FBOXTEXT, EDITABLE, NORMAL, xf, yport, 6*lm.cw, lm.rh);
    fp_dlg[FP_PORT_EDIT].ob_spec.tedinfo = &ti_fp_port;

    set_obj(fp_dlg, FP_LBL_MOUNT, G_STRING, NONE, NORMAL, xl, ymount, 11*lm.cw, lm.rh);
    fp_dlg[FP_LBL_MOUNT].ob_spec.free_string = "Mount dir:";
    set_obj(fp_dlg, FP_MOUNT_EDIT, G_FBOXTEXT, EDITABLE, NORMAL, xf, ymount, 23*lm.cw, lm.rh);
    fp_dlg[FP_MOUNT_EDIT].ob_spec.tedinfo = &ti_fp_mount;

    /* Empty mount_path is valid ("server root"), same convention
     * SIDETNFS-Config's TNFS drive editor uses. */
    set_obj(fp_dlg, FP_MOUNT_HINT, G_STRING, NONE, NORMAL, xf, ymounthint, 30*lm.cw, lm.rh);
    fp_dlg[FP_MOUNT_HINT].ob_spec.free_string = "empty = server root";

    set_obj(fp_dlg, FP_LBL_SDPATH, G_STRING, NONE, NORMAL, xl, ysdpath, 11*lm.cw, lm.rh);
    fp_dlg[FP_LBL_SDPATH].ob_spec.free_string = "SD folder:";
    /* te_txtlen/te_tmplen (set by init_ti() above) allow the full 255
     * characters; the on-screen box is narrower and scrolls, standard GEM
     * text-field behavior when the buffer is longer than the display. */
    set_obj(fp_dlg, FP_SDPATH_EDIT, G_FBOXTEXT, EDITABLE, NORMAL, xf, ysdpath, 31*lm.cw, lm.rh);
    fp_dlg[FP_SDPATH_EDIT].ob_spec.tedinfo = &ti_fp_sdpath;

    set_obj(fp_dlg, FP_LBL_ACTIVE, G_STRING, NONE, NORMAL, xl, yactive, 11*lm.cw, lm.rh);
    fp_dlg[FP_LBL_ACTIVE].ob_spec.free_string = "Status:";
    set_obj(fp_dlg, FP_ACTIVE_BTN, G_BUTTON, EXIT | TOUCHEXIT, NORMAL, xf, yactive, 10*lm.cw, lm.rh);
    fp_dlg[FP_ACTIVE_BTN].ob_spec.free_string = buf_fp_active;

    set_obj(fp_dlg, FP_DIV2, G_BOX, NONE, NORMAL, lm.cw, ydiv2, DW - 2*lm.cw, 2);
    fp_dlg[FP_DIV2].ob_spec.index = 0x00001171L;

    set_obj(fp_dlg, FP_DELETE, G_BUTTON, EXIT | TOUCHEXIT, show_delete ? NORMAL : DISABLED,
            2*lm.cw, ybtn, 9*lm.cw, lm.rh);
    fp_dlg[FP_DELETE].ob_spec.free_string = " Remove  ";

    set_obj(fp_dlg, FP_OK, G_BUTTON, EXIT | DEFAULT | TOUCHEXIT, NORMAL, 22*lm.cw, ybtn, 8*lm.cw, lm.rh);
    fp_dlg[FP_OK].ob_spec.free_string = "   OK   ";

    set_obj(fp_dlg, FP_CANCEL, G_BUTTON, EXIT | TOUCHEXIT, NORMAL, 33*lm.cw, ybtn, 8*lm.cw, lm.rh);
    fp_dlg[FP_CANCEL].ob_spec.free_string = " Cancel ";

    wire_tree(fp_dlg, FP_NOBJS);

    /* Snapshot each toggle-affected object's real geometry now, before
     * fp_apply_backend_visibility() ever zero-sizes the currently-hidden
     * block -- see fp_tnfs_geom's own comment. */
    {
        int i;
        for (i = 0; i < FP_TNFS_OBJS_COUNT; i++) {
            fp_tnfs_geom[i][0] = fp_dlg[fp_tnfs_objs[i]].ob_x;
            fp_tnfs_geom[i][1] = fp_dlg[fp_tnfs_objs[i]].ob_y;
            fp_tnfs_geom[i][2] = fp_dlg[fp_tnfs_objs[i]].ob_width;
            fp_tnfs_geom[i][3] = fp_dlg[fp_tnfs_objs[i]].ob_height;
        }
        for (i = 0; i < FP_SD_OBJS_COUNT; i++) {
            fp_sd_geom[i][0] = fp_dlg[fp_sd_objs[i]].ob_x;
            fp_sd_geom[i][1] = fp_dlg[fp_sd_objs[i]].ob_y;
            fp_sd_geom[i][2] = fp_dlg[fp_sd_objs[i]].ob_width;
            fp_sd_geom[i][3] = fp_dlg[fp_sd_objs[i]].ob_height;
        }
    }
}

static void fp_load_from_profile(const Profile *p, int is_new)
{
    char port_str[FP_BUF_PORT];

    set_buf(buf_fp_nick,   FP_BUF_NICK,   p->nickname);
    set_buf(buf_fp_host,   FP_BUF_HOST,   p->host);
    sprintf(port_str, "%d", p->port);
    set_buf(buf_fp_port,   FP_BUF_PORT,   port_str);
    set_buf(buf_fp_mount,  FP_BUF_MOUNT,  p->mount_path);
    set_buf(buf_fp_sdpath, FP_BUF_SDPATH, p->sd_path);

    /* A new (EMPTY) slot defaults to Active/ENABLED and Source: TNFS
     * unless the user explicitly changes either before OK -- same
     * "active by default" convention SIDETNFS-Config's TNFS drive editor
     * uses. */
    fp_editor_enabled = is_new ? 1 : (p->state == PROFILE_SLOT_ENABLED);
    fp_editor_backend = is_new ? PROFILE_BACKEND_TNFS : p->backend;
    update_fp_active_button_text();
    update_fp_source_buttons();
    fp_apply_backend_visibility();
}

static void fp_save_to_profile(Profile *p)
{
    buf_copy(buf_fp_nick,   p->nickname,   PROFILE_NICK_LEN);
    buf_copy(buf_fp_host,   p->host,       PROFILE_HOST_LEN);
    buf_copy(buf_fp_mount,  p->mount_path, PROFILE_MOUNT_LEN);
    buf_copy(buf_fp_sdpath, p->sd_path,    PROFILE_SDPATH_LEN);

    if (p->mount_path[0] == '\0') {
        p->mount_path[0] = '/';
        p->mount_path[1] = '\0';
    }

    p->port    = atoi(buf_fp_port); /* range-checked by validate_profile() when backend == TNFS */
    p->state   = fp_editor_enabled ? PROFILE_SLOT_ENABLED : PROFILE_SLOT_DISABLED;
    p->backend = fp_editor_backend;
}

/* Returns 2 if the profile was removed (cleared to EMPTY), 1 if
 * added/modified, 0 if cancelled without changes. index is always a
 * valid fixed slot 0..MAX_PROFILES-1. */
/* Is obj one of the 5 EDITABLE text fields on fp_dlg? Guards the custom
 * loop's own focus-tracking below: objc_edit() assumes ob_spec.tedinfo,
 * and calling it on some other object (a G_STRING label, a button) would
 * read that object's ob_spec union as a TEDINFO pointer it never is --
 * exactly the kind of object-state mistake this project's own past
 * real-hardware crashes came from, so this is checked explicitly rather
 * than assumed. */
static int fp_is_editable(short obj)
{
    return obj == FP_NICK_EDIT || obj == FP_HOST_EDIT || obj == FP_PORT_EDIT
        || obj == FP_MOUNT_EDIT || obj == FP_SDPATH_EDIT;
}

/* Custom evnt_multi() loop instead of dialog_click()/form_do() -- needed
 * for the Esc/F1/F2/Ctrl+A/Ctrl+R shortcuts below, none of which form_do()
 * has any way to expose to the caller. Unlike the FS_ and FE_ dialogs
 * (server_selector_run()/edit_servers_run()), this dialog has real
 * EDITABLE text fields, so a
 * plain form_button()-only loop isn't enough -- text editing itself is
 * driven by AES's own form_keybd() + objc_edit(), the exact same pair
 * form_do() uses internally to do it (form_keybd() decides whether a key
 * moves focus to a different field or an EXIT object, or should be
 * inserted into the current one; objc_edit() actually inserts/deletes/
 * moves the cursor). This pairing is a well-established, working pattern
 * for a form_do() replacement that still needs live text fields -- not
 * this project's own invention. */
static int fp_editor_run(ProfileConfig *cfg, int index)
{
    DialogGeometry geo;
    short which;
    int done;
    int is_new = profile_slot_is_empty(&cfg->profiles[index]);
    Profile working;
    char msg[200];

    short edit_ob, next_ob, idx;
    short mx, my, mb, ks, kr, br;
    short kmsg[8];
    short event, obj;
    int cont;

    if (is_new) {
        memset(&working, 0, sizeof(working));
        working.backend = PROFILE_BACKEND_TNFS;
        working.port = 16384; /* matches the reused firmware's own TNFS default */
        working.mount_path[0] = '/';
        working.mount_path[1] = '\0';
    } else {
        working = cfg->profiles[index];
    }

    fp_dialog_init(!is_new);
    fp_load_from_profile(&working, is_new);
    dialog_open(fp_dlg, FP_ROOT, &geo, 0);

    /* Auto-focus Nickname the instant the dialog opens, same as the
     * original form_do()-based version. Unlike that version, this is one
     * continuous loop rather than a series of separate form_do() calls,
     * so there is only this one "first" moment to handle -- see the
     * FP_SOURCE_TNFS_BTN/FP_SOURCE_SD_BTN/FP_ACTIVE_BTN cases below for
     * how focus is kept (not reset to "nothing") across those toggles now. */
    edit_ob = FP_NICK_EDIT;
    next_ob = FP_ROOT;
    objc_edit(fp_dlg, edit_ob, 0, &idx, ED_INIT);

    done = 0;
    while (!done) {
        if (next_ob != FP_ROOT && next_ob != edit_ob && fp_is_editable(next_ob)) {
            /* Focus moved to a different editable field (Tab/Shift-Tab
             * via form_keybd(), or a mouse click into one via
             * form_button()) -- turn the old cursor off, the new one on. */
            objc_edit(fp_dlg, edit_ob, 0, &idx, ED_END);
            edit_ob = next_ob;
            objc_edit(fp_dlg, edit_ob, 0, &idx, ED_INIT);
        }
        next_ob = FP_ROOT;
        which = 0;
        cont = 1;

        event = evnt_multi(MU_KEYBD | MU_BUTTON,
                           2, 1, 1,
                           0, 0, 0, 0, 0,
                           0, 0, 0, 0, 0,
                           kmsg,
                           0UL,
                           &mx, &my, &mb, &ks, &kr, &br);

        if (event & MU_KEYBD) {
            int scan = (kr >> 8) & 0x00FF;
            int ascii = kr & 0x00FF;

            if (scan == 0x01 || ascii == 0x1B) {
                /* Esc: same as [Cancel]. */
                which = FP_CANCEL;
            } else if (scan == 0x3B) {
                /* F1: same as [TNFS] -- swap Source to TNFS. */
                which = FP_SOURCE_TNFS_BTN;
            } else if (scan == 0x3C) {
                /* F2: same as [SD card] -- swap Source to SD card. */
                which = FP_SOURCE_SD_BTN;
            } else if (scan == 0x1E && (ks & K_CTRL)) {
                /* Ctrl+A: same as [Status] -- swap Active <-> Inactive.
                 * Ctrl, not plain A, so typing an actual "a" into
                 * Nickname/Host/etc. keeps working normally. */
                which = FP_ACTIVE_BTN;
            } else if (scan == 0x13 && (ks & K_CTRL)) {
                /* Ctrl+R: same as [Remove]. Ctrl, not plain R, for the
                 * same reason as Ctrl+A above. */
                which = FP_DELETE;
            } else {
                /* Not one of ours -- hand it to AES's own field-editing
                 * dispatcher (see this function's own header comment). */
                cont = form_keybd(fp_dlg, edit_ob, next_ob, kr, &next_ob, &kr);
                if (kr != 0)
                    objc_edit(fp_dlg, edit_ob, kr, &idx, ED_CHAR);
                if (!cont)
                    which = next_ob; /* e.g. Return anywhere activating FP_OK's own DEFAULT flag */
            }
        }

        if (event & MU_BUTTON) {
            obj = objc_find(fp_dlg, FP_ROOT, MAX_DEPTH, mx, my);
            if (obj > 0) {
                cont = form_button(fp_dlg, obj, br, &next_ob);
                if (!cont)
                    which = next_ob;
            }
        }

        if (which == 0)
            continue; /* nothing to act on this iteration -- e.g. a field just got a character or lost/gained focus */

        switch (which) {
        case FP_SOURCE_TNFS_BTN:
            fp_editor_backend = PROFILE_BACKEND_TNFS;
            update_fp_source_buttons();
            fp_apply_backend_visibility();
            /* Cursor off/on around the redraw, same reasoning as any
             * field-content-changing redraw needs (see this function's
             * own header comment on the objc_edit()/form_keybd() pairing)
             * -- but unlike the original form_do()-based version, focus
             * is kept on the SAME field instead of being dropped, so the
             * user can keep typing right after toggling Source without
             * having to click back into a field. */
            objc_edit(fp_dlg, edit_ob, 0, &idx, ED_END);
            objc_draw(fp_dlg, FP_ROOT, MAX_DEPTH, geo.x, geo.y, geo.w, geo.h);
            objc_edit(fp_dlg, edit_ob, 0, &idx, ED_INIT);
            break;

        case FP_SOURCE_SD_BTN:
            fp_editor_backend = PROFILE_BACKEND_SD;
            update_fp_source_buttons();
            fp_apply_backend_visibility();
            objc_edit(fp_dlg, edit_ob, 0, &idx, ED_END);
            objc_draw(fp_dlg, FP_ROOT, MAX_DEPTH, geo.x, geo.y, geo.w, geo.h);
            objc_edit(fp_dlg, edit_ob, 0, &idx, ED_INIT);
            break;

        case FP_ACTIVE_BTN:
            fp_editor_enabled = !fp_editor_enabled;
            update_fp_active_button_text();
            objc_edit(fp_dlg, edit_ob, 0, &idx, ED_END);
            objc_draw(fp_dlg, FP_ROOT, MAX_DEPTH, geo.x, geo.y, geo.w, geo.h);
            objc_edit(fp_dlg, edit_ob, 0, &idx, ED_INIT);
            break;

        case FP_DELETE:
            if (!is_new) {
                objc_edit(fp_dlg, edit_ob, 0, &idx, ED_END);
                if (form_alert(1, "[2][Remove this floppy source?][Remove|Cancel]") == 1) {
                    done = 2;
                } else {
                    objc_edit(fp_dlg, edit_ob, 0, &idx, ED_INIT);
                }
            }
            break;

        case FP_OK:
            objc_edit(fp_dlg, edit_ob, 0, &idx, ED_END);
            fp_save_to_profile(&working);
            if (!validate_profile(&working, msg)) {
                form_alert(1, msg);
                objc_edit(fp_dlg, edit_ob, 0, &idx, ED_INIT);
                break;
            }
            cfg->profiles[index] = working;
            done = 1;
            break;

        case FP_CANCEL:
        default:
            objc_edit(fp_dlg, edit_ob, 0, &idx, ED_END);
            done = 3;
            break;
        }
    }

    dialog_close(&geo);

    if (done == 2) {
        memset(&cfg->profiles[index], 0, sizeof(cfg->profiles[index])); /* state == PROFILE_SLOT_EMPTY */
        if (cfg->active_index == index) {
            cfg->active_index = 0;
            favcfg_write_active_slot(1); /* the remembered slot just got deleted -- don't leave it pointing at a stale slot */
        }
        return 2;
    }
    if (done == 1) return 1;
    return 0;
}

/* ================================================================== */
/* Edit Servers overview (FE_*)                                        */
/* Inspired by SIDETNFS-Config's Drives dialog (OV_*): eight fixed rows, */
/* each with its own Add/Edit button. No SETTINGS-disk row (this project */
/* has no GEMDOS drives at all).                                         */
/* ================================================================== */
enum {
    FE_ROOT = 0,
    FE_TITLE,
    FE_DIV1,
    FE_ROW_BASE
};
#define FE_ROW_TEXT(i) (FE_ROW_BASE + (i)*2 + 0)
#define FE_ROW_BTN(i)  (FE_ROW_BASE + (i)*2 + 1)
#define FE_AFTER_ROWS  (FE_ROW_BASE + MAX_PROFILES*2)
#define FE_DIV2   (FE_AFTER_ROWS + 0)
#define FE_SAVE   (FE_AFTER_ROWS + 1)
#define FE_OK     (FE_AFTER_ROWS + 2)
#define FE_CANCEL (FE_AFTER_ROWS + 3)
#define FE_NOBJS  (FE_AFTER_ROWS + 4)
static OBJECT fe_dlg[FE_NOBJS];

#define FE_ROW_BUF 44
static char fe_row_text[MAX_PROFILES][FE_ROW_BUF];

static void fe_dialog_init(void)
{
    LayoutMetrics lm;
    int DW, DH;
    int yt, ydiv1, yrow0, ydiv2, ybtn;
    int i;

    layout_metrics_get(&lm);

    /* 50 chars wide -- matches SIDETNFS-Config's own widest dialogs
     * (RESEARCH-STEP0.md section 3.3), needed here to fit the longer row
     * text ("1* Active   [TNFS] Nickname...", ~36 chars) added for the SD
     * backend. */
    DW = 50 * lm.cw;
    yt    = lm.tm;
    ydiv1 = yt + lm.rh + 1;
    yrow0 = ydiv1 + 5;
    ydiv2 = yrow0 + MAX_PROFILES * lm.pitch + 2;
    ybtn  = ydiv2 + 7;
    DH    = ybtn + lm.rh + lm.tm + 3;

    set_obj(fe_dlg, FE_ROOT, G_BOX, NONE, NORMAL, 0, 0, DW, DH);
    fe_dlg[FE_ROOT].ob_spec.index = 0x00031070L;

    set_obj(fe_dlg, FE_TITLE, G_STRING, NONE, NORMAL, 14*lm.cw, yt, 22*lm.cw, lm.rh);
    fe_dlg[FE_TITLE].ob_spec.free_string = "Edit Floppy Sources";

    set_obj(fe_dlg, FE_DIV1, G_BOX, NONE, NORMAL, lm.cw, ydiv1, DW - 2*lm.cw, 2);
    fe_dlg[FE_DIV1].ob_spec.index = 0x00001171L;

    for (i = 0; i < MAX_PROFILES; i++) {
        int ry = yrow0 + i * lm.pitch;

        set_obj(fe_dlg, FE_ROW_TEXT(i), G_STRING, NONE, NORMAL, lm.cw, ry, 38*lm.cw, lm.rh);
        fe_dlg[FE_ROW_TEXT(i)].ob_spec.free_string = fe_row_text[i];

        set_obj(fe_dlg, FE_ROW_BTN(i), G_BUTTON, EXIT | TOUCHEXIT, NORMAL, 40*lm.cw, ry, 8*lm.cw, lm.rh);
        fe_dlg[FE_ROW_BTN(i)].ob_spec.free_string = "  Add   ";
    }

    set_obj(fe_dlg, FE_DIV2, G_BOX, NONE, NORMAL, lm.cw, ydiv2, DW - 2*lm.cw, 2);
    fe_dlg[FE_DIV2].ob_spec.index = 0x00001171L;

    set_obj(fe_dlg, FE_SAVE, G_BUTTON, EXIT | TOUCHEXIT, NORMAL, 2*lm.cw, ybtn, 8*lm.cw, lm.rh);
    fe_dlg[FE_SAVE].ob_spec.free_string = "  Save  ";

    set_obj(fe_dlg, FE_OK, G_BUTTON, EXIT | DEFAULT | TOUCHEXIT, NORMAL, 19*lm.cw, ybtn, 8*lm.cw, lm.rh);
    fe_dlg[FE_OK].ob_spec.free_string = "   OK   ";

    set_obj(fe_dlg, FE_CANCEL, G_BUTTON, EXIT | TOUCHEXIT, NORMAL, 30*lm.cw, ybtn, 8*lm.cw, lm.rh);
    fe_dlg[FE_CANCEL].ob_spec.free_string = " Cancel ";

    wire_tree(fe_dlg, FE_NOBJS);
}

/* Eight fixed rows, always -- an EMPTY slot shows only its slot number and
 * an "Add" button; a configured slot shows slot/state/nickname and an
 * "Edit" button. Never reordered. */
static void fe_refresh_rows(const ProfileConfig *cfg)
{
    int i;

    for (i = 0; i < MAX_PROFILES; i++) {
        const Profile *p = &cfg->profiles[i];

        if (profile_slot_is_empty(p)) {
            sprintf(fe_row_text[i], "%d  Empty", i + 1);
            fe_dlg[FE_ROW_BTN(i)].ob_spec.free_string = "  Add   ";
        } else {
            const char *state_word = profile_slot_is_enabled(p) ? "Active" : "Inactive";
            const char *backend_word = (p->backend == PROFILE_BACKEND_SD) ? "SD  " : "TNFS";
            const char *active_mark = (i == cfg->active_index) ? "*" : " ";
            sprintf(fe_row_text[i], "%d%s %-8s [%s] %-17.17s", i + 1, active_mark, state_word, backend_word, p->nickname);
            fe_dlg[FE_ROW_BTN(i)].ob_spec.free_string = "  Edit  ";
        }
    }
}

/* Keyboard-navigation highlight for FE_ROW_BTN, same idiom/reasoning as
 * fs_selected_row/fs_redraw_row above -- FE_ROW_BTN is also a plain
 * EXIT|TOUCHEXIT button, not SELECTABLE|RBUTTON, so the highlight is a
 * direct ob_state SELECTED toggle + plain objc_draw(), not
 * form_button()'s own RBUTTON machinery. */
static int fe_selected_row = 0;

static void fe_redraw_row(int row)
{
    OBJECT *ro = &fe_dlg[FE_ROW_BTN(row)];
    short x = (short)(fe_dlg[FE_ROOT].ob_x + ro->ob_x);
    short y = (short)(fe_dlg[FE_ROOT].ob_y + ro->ob_y);

    objc_draw(fe_dlg, FE_ROW_BTN(row), MAX_DEPTH, x, y, ro->ob_width, ro->ob_height);
}

/* Custom evnt_multi() loop instead of dialog_click()/form_do() -- same
 * reasoning as server_selector_run()'s own comment: needed for Up/Down
 * row navigation and the digit/S/Esc keyboard shortcuts below. */
static void edit_servers_run(ProfileConfig *cfg)
{
    DialogGeometry geo;
    short mx, my, mb, ks, kr, br;
    short msg[8];
    short event, obj, next, which;
    int done;
    char save_msg[220];

    fe_dialog_init();
    fe_refresh_rows(cfg);
    dialog_open(fe_dlg, FE_ROOT, &geo, 0);

    fe_selected_row = 0;
    fe_dlg[FE_ROW_BTN(fe_selected_row)].ob_state |= (unsigned short)SELECTED;
    fe_redraw_row(fe_selected_row);

    done = 0;
    while (!done) {
        event = evnt_multi(MU_KEYBD | MU_BUTTON,
                           2, 1, 1,
                           0, 0, 0, 0, 0,
                           0, 0, 0, 0, 0,
                           msg,
                           0UL,
                           &mx, &my, &mb, &ks, &kr, &br);

        if (event & MU_BUTTON) {
            obj = objc_find(fe_dlg, FE_ROOT, MAX_DEPTH, mx, my);
            if (obj > 0) {
                next = obj;
                if (!form_button(fe_dlg, obj, br, &next)) {
                    which = (short)(next & 0x7FFF);
                    if (which == FE_SAVE) {
                        perform_save(cfg, save_msg);
                        form_alert(1, save_msg);
                    } else if (which >= FE_ROW_BASE && which < FE_AFTER_ROWS) {
                        int obj_offset = which - FE_ROW_BASE;
                        int slot = obj_offset / 2;
                        int is_btn = (obj_offset % 2) == 1;
                        if (is_btn) {
                            fe_dlg[FE_ROW_BTN(fe_selected_row)].ob_state &= (unsigned short)(~SELECTED);
                            fe_selected_row = slot;
                            fp_editor_run(cfg, slot);
                            fe_refresh_rows(cfg);
                            fe_dlg[FE_ROW_BTN(fe_selected_row)].ob_state |= (unsigned short)SELECTED;
                            objc_draw(fe_dlg, FE_ROOT, MAX_DEPTH, geo.x, geo.y, geo.w, geo.h);
                        }
                    } else if (which == FE_OK || which == FE_CANCEL) {
                        done = 1;
                    }
                }
            }
        }

        if (event & MU_KEYBD) {
            int scan = (kr >> 8) & 0x00FF;
            int ascii = kr & 0x00FF;

            if (scan == 0x48 || scan == 0x50) { /* up / down */
                int new_row = fe_selected_row;

                if (scan == 0x48) /* up */
                    new_row = (new_row <= 0) ? 0 : new_row - 1;
                else /* down */
                    new_row = (new_row >= MAX_PROFILES - 1) ? MAX_PROFILES - 1 : new_row + 1;

                if (new_row != fe_selected_row) {
                    fe_dlg[FE_ROW_BTN(fe_selected_row)].ob_state &= (unsigned short)(~SELECTED);
                    fe_redraw_row(fe_selected_row);
                    fe_selected_row = new_row;
                    fe_dlg[FE_ROW_BTN(fe_selected_row)].ob_state |= (unsigned short)SELECTED;
                    fe_redraw_row(fe_selected_row);
                }
            } else if (ascii == ' ' || (ascii >= '1' && ascii <= '8')) {
                /* Space: same as clicking the highlighted row's own
                 * Edit/Add button. A digit 1-8 does the same but for
                 * that specific slot directly, regardless of which row
                 * is currently highlighted -- moves the highlight there
                 * too, so a following Up/Down continues from it. Space,
                 * not Enter: Up/Down already claims the row list, and
                 * Enter is left free to mean [OK] (below), same as it
                 * would via plain form_do()'s own DEFAULT-button
                 * convention -- otherwise OK becomes unreachable from the
                 * keyboard once a row is highlighted. */
                int slot = (ascii == ' ') ? fe_selected_row : (ascii - '1');

                fe_dlg[FE_ROW_BTN(fe_selected_row)].ob_state &= (unsigned short)(~SELECTED);
                fe_selected_row = slot;
                fp_editor_run(cfg, slot);
                fe_refresh_rows(cfg);
                fe_dlg[FE_ROW_BTN(fe_selected_row)].ob_state |= (unsigned short)SELECTED;
                objc_draw(fe_dlg, FE_ROOT, MAX_DEPTH, geo.x, geo.y, geo.w, geo.h);
            } else if (scan == 0x1F) {
                /* S: same as [Save] -- scan code, standard AT/Atari S. */
                perform_save(cfg, save_msg);
                form_alert(1, save_msg);
            } else if (ascii == 0x0D) {
                /* Enter: same as [OK]. */
                done = 1;
            } else if (scan == 0x01 || ascii == 0x1B) {
                /* Esc: same as [Cancel] -- scan code and ASCII both
                 * checked, same dual-check convention established
                 * elsewhere in this file. */
                done = 1;
            }
        }
    }

    dialog_close(&geo);
}

/* ================================================================== */
/* Server selector (FS_*)                                              */
/* [Server] -> pick one of up to 8 configured profiles as the active one, */
/* or jump into the Edit Servers overview above. Every row is a button so */
/* Add/Edit never needs to live here too -- an EMPTY slot's button is     */
/* simply DISABLED (same convention SIDETNFS-Config's TE_DELETE uses for  */
/* "not applicable to this slot").                                       */
/* ================================================================== */
enum {
    FS_ROOT = 0,
    FS_TITLE,
    FS_DIV1,
    FS_ROW_BASE
};
#define FS_ROW(i)      (FS_ROW_BASE + (i))
#define FS_AFTER_ROWS  (FS_ROW_BASE + MAX_PROFILES)
#define FS_DIV2   (FS_AFTER_ROWS + 0)
#define FS_EDIT   (FS_AFTER_ROWS + 1)
#define FS_CANCEL (FS_AFTER_ROWS + 2)
#define FS_NOBJS  (FS_AFTER_ROWS + 3)
static OBJECT fs_dlg[FS_NOBJS];

#define FS_ROW_BUF 32
static char fs_row_text[MAX_PROFILES][FS_ROW_BUF];

static void fs_dialog_init(void)
{
    LayoutMetrics lm;
    int DW, DH;
    int yt, ydiv1, yrow0, ydiv2, ybtn;
    int i;

    layout_metrics_get(&lm);

    DW = 34 * lm.cw;
    yt    = lm.tm;
    ydiv1 = yt + lm.rh + 1;
    yrow0 = ydiv1 + 5;
    ydiv2 = yrow0 + MAX_PROFILES * lm.pitch + 2;
    ybtn  = ydiv2 + 7;
    DH    = ybtn + lm.rh + lm.tm + 3;

    set_obj(fs_dlg, FS_ROOT, G_BOX, NONE, NORMAL, 0, 0, DW, DH);
    fs_dlg[FS_ROOT].ob_spec.index = 0x00031070L;

    set_obj(fs_dlg, FS_TITLE, G_STRING, NONE, NORMAL, 9*lm.cw, yt, 16*lm.cw, lm.rh);
    fs_dlg[FS_TITLE].ob_spec.free_string = "Floppy Servers";

    set_obj(fs_dlg, FS_DIV1, G_BOX, NONE, NORMAL, lm.cw, ydiv1, DW - 2*lm.cw, 2);
    fs_dlg[FS_DIV1].ob_spec.index = 0x00001171L;

    for (i = 0; i < MAX_PROFILES; i++) {
        int ry = yrow0 + i * lm.pitch;
        set_obj(fs_dlg, FS_ROW(i), G_BUTTON, EXIT | TOUCHEXIT, NORMAL, lm.cw, ry, 32*lm.cw, lm.rh);
        fs_dlg[FS_ROW(i)].ob_spec.free_string = fs_row_text[i];
    }

    set_obj(fs_dlg, FS_DIV2, G_BOX, NONE, NORMAL, lm.cw, ydiv2, DW - 2*lm.cw, 2);
    fs_dlg[FS_DIV2].ob_spec.index = 0x00001171L;

    set_obj(fs_dlg, FS_EDIT, G_BUTTON, EXIT | TOUCHEXIT, NORMAL, 2*lm.cw, ybtn, 16*lm.cw, lm.rh);
    fs_dlg[FS_EDIT].ob_spec.free_string = "Edit sources...";

    set_obj(fs_dlg, FS_CANCEL, G_BUTTON, EXIT | TOUCHEXIT, NORMAL, 24*lm.cw, ybtn, 8*lm.cw, lm.rh);
    fs_dlg[FS_CANCEL].ob_spec.free_string = " Cancel ";

    wire_tree(fs_dlg, FS_NOBJS);
}

static void fs_refresh_rows(const ProfileConfig *cfg)
{
    int i;

    for (i = 0; i < MAX_PROFILES; i++) {
        const Profile *p = &cfg->profiles[i];

        if (profile_slot_is_configured(p)) {
            const char *tag = (p->backend == PROFILE_BACKEND_SD) ? "[SD]    " : "[Server]";
            sprintf(fs_row_text[i], "%s%s %-.18s", (i == cfg->active_index) ? "> " : "  ", tag, p->nickname);
            fs_dlg[FS_ROW(i)].ob_state &= (unsigned short)(~DISABLED);
        } else {
            sprintf(fs_row_text[i], "  -- empty --");
            fs_dlg[FS_ROW(i)].ob_state |= (unsigned short)DISABLED;
        }
    }
}

/* Keyboard-navigation highlight (Up/Down/Enter, see server_selector_run()'s
 * own custom event loop) -- distinct from a "click selects" model like
 * FA_ROW/FM_ROW: FS_ROW is a plain EXIT|TOUCHEXIT button, a single click
 * already activates it immediately, so this is purely a visual "which row
 * would Enter/a digit act on" cursor, managed by hand (a direct ob_state
 * SELECTED toggle + plain objc_draw()) rather than form_button()'s own
 * RBUTTON-toggle machinery, which only applies to SELECTABLE|RBUTTON
 * objects like FA_ROW -- FS_ROW is neither. G_BUTTON paints its own full
 * fill on every objc_draw(), unlike G_STRING, so no v_bar erase-first step
 * is needed here the way FA_ROW/FM_ROW need one for their own highlight. */
static int fs_selected_row = 0;

static void fs_redraw_row(int row)
{
    OBJECT *ro = &fs_dlg[FS_ROW(row)];
    short x = (short)(fs_dlg[FS_ROOT].ob_x + ro->ob_x);
    short y = (short)(fs_dlg[FS_ROOT].ob_y + ro->ob_y);

    objc_draw(fs_dlg, FS_ROW(row), MAX_DEPTH, x, y, ro->ob_width, ro->ob_height);
}

/* Returns 1 if the user picked a profile (cfg->active_index updated,
 * locally only -- see perform_save()'s own comment on when this actually
 * reaches flash), 2 if the user asked to edit the server list instead
 * (caller then calls edit_servers_run()), 0 if cancelled.
 *
 * Custom evnt_multi() loop instead of dialog_click()/form_do() -- needed
 * for Up/Down row navigation and the E/digit-free keyboard shortcuts
 * below, same reasoning fm_form_do_events()'s own comment gives for why
 * form_do() alone isn't enough. */
static int server_selector_run(ProfileConfig *cfg)
{
    DialogGeometry geo;
    short mx, my, mb, ks, kr, br;
    short msg[8];
    short event, obj, next, which;
    int result = 0;
    int done = 0;

    fs_dialog_init();
    fs_refresh_rows(cfg);
    dialog_open(fs_dlg, FS_ROOT, &geo, 0);

    /* Start the highlight on the currently active profile's own row, if
     * any, so Enter/arrows work immediately without an extra keypress. */
    fs_selected_row = (cfg->active_index >= 0 && cfg->active_index < MAX_PROFILES) ? cfg->active_index : 0;
    fs_dlg[FS_ROW(fs_selected_row)].ob_state |= (unsigned short)SELECTED;
    fs_redraw_row(fs_selected_row);

    while (!done) {
        event = evnt_multi(MU_KEYBD | MU_BUTTON,
                           2, 1, 1,
                           0, 0, 0, 0, 0,
                           0, 0, 0, 0, 0,
                           msg,
                           0UL,
                           &mx, &my, &mb, &ks, &kr, &br);

        if (event & MU_BUTTON) {
            obj = objc_find(fs_dlg, FS_ROOT, MAX_DEPTH, mx, my);
            if (obj > 0) {
                next = obj;
                if (!form_button(fs_dlg, obj, br, &next)) {
                    which = (short)(next & 0x7FFF);
                    if (which >= FS_ROW_BASE && which < FS_AFTER_ROWS) {
                        int slot = which - FS_ROW_BASE;
                        if (profile_slot_is_configured(&cfg->profiles[slot])) {
                            cfg->active_index = slot;
                            favcfg_write_active_slot(slot + 1); /* remembered independently of the firmware's own flash-only active-profile write, see favcfg_write_active_slot()'s own comment */
                            result = 1;
                            done = 1;
                        }
                    } else if (which == FS_EDIT) {
                        result = 2;
                        done = 1;
                    } else if (which == FS_CANCEL) {
                        result = 0;
                        done = 1;
                    }
                }
            }
        }

        if (event & MU_KEYBD) {
            int scan = (kr >> 8) & 0x00FF;

            if (scan == 0x48 || scan == 0x50) { /* up / down */
                int new_row = fs_selected_row;

                if (scan == 0x48) /* up */
                    new_row = (new_row <= 0) ? 0 : new_row - 1;
                else /* down */
                    new_row = (new_row >= MAX_PROFILES - 1) ? MAX_PROFILES - 1 : new_row + 1;

                if (new_row != fs_selected_row) {
                    fs_dlg[FS_ROW(fs_selected_row)].ob_state &= (unsigned short)(~SELECTED);
                    fs_redraw_row(fs_selected_row);
                    fs_selected_row = new_row;
                    fs_dlg[FS_ROW(fs_selected_row)].ob_state |= (unsigned short)SELECTED;
                    fs_redraw_row(fs_selected_row);
                }
            } else if ((kr & 0x00FF) == 0x0D) {
                /* Enter: same as clicking the highlighted row -- activates
                 * it and closes, same as FS_ROW's own EXIT|TOUCHEXIT click.
                 * A no-op on an empty (DISABLED) slot, matching the mouse's
                 * own behavior there. */
                if (profile_slot_is_configured(&cfg->profiles[fs_selected_row])) {
                    cfg->active_index = fs_selected_row;
                    favcfg_write_active_slot(fs_selected_row + 1);
                    result = 1;
                    done = 1;
                }
            } else if (scan == 0x12) {
                /* E: same as [Edit sources...] -- scan code, standard
                 * AT/Atari E. */
                result = 2;
                done = 1;
            } else if (scan == 0x01 || (kr & 0x00FF) == 0x1B) {
                /* Esc: same as [Cancel] -- scan code and ASCII both
                 * checked, same dual-check convention established
                 * elsewhere in this file (see fm_form_do_events()'s own
                 * Esc handling). */
                result = 0;
                done = 1;
            }
        }
    }

    dialog_close(&geo);
    return result;
}

/* ================================================================== */
/* Favorites dialog (FA_*)                                             */
/* This is now the app's own top-level/home screen -- dialog_run() (FM_*   */
/* section below) runs ITS loop directly rather than the browser's, and    */
/* opens the browser (fm_browse_run()) as a NESTED call whenever [Browser] */
/* is clicked, to go pick a file. A file confirmed there (double-click,    */
/* Return/Enter, or the browser's own [Add] button) arms PLACE MODE       */
/* on return; pressing [Back] there returns with nothing armed instead.    */
/* No real favorites storage/move/delete/start behind any of this yet, per */
/* this project's own task briefs so far. */
/*                                                                       */
/* Deliberately built to be pixel-identical in width/height to the main  */
/* browser window (see std_frame_get(), factored out for exactly this   */
/* purpose) -- including going borderless/fullscreen on a medium-res-    */
/* height screen the same way. The visual "this is a different part of   */
/* the app" signal comes from the CONTENT (tabs instead of a source      */
/* line, a plain list instead of selectable rows, a different button     */
/* row), not from a differently-shaped window -- and a same-shape window */
/* means dialog placement/behavior never surprises the user switching    */
/* between the two. The main window's own fm_dialog_init() is untouched  */
/* by this -- std_frame_get() only mirrors its math, nothing calls into  */
/* fm_dialog_init() itself.                                              */
/*                                                                       */
/* Rows are SELECTABLE|RBUTTON (same family FM_ROW uses) in NORMAL MODE:    */
/* a single click selects (fa_selected_row) via AES's own object dispatch, */
/* a double-click on an occupied slot shows a "Now starting <name>" stub   */
/* alert (still no real mount/emulation behind it). In PLACE MODE, row     */
/* clicks are instead hit-tested manually (fa_row_at()) and bypass AES     */
/* dispatch entirely, since a row's meaning there is placement, not        */
/* selection -- see the MU_BUTTON handling below for how the two are kept  */
/* apart. PLACE MODE: opened already-armed with the browser's selected     */
/* filename (fa_place_name), FLAT_HAND shown while the mouse is over the   */
/* row list (MU_M1 enter/leave watch), and a row click stores the filename */
/* into that favorite slot (persisted to disk, see src/favcfg.c), redraws  */
/* it, and returns to normal. Erase empties the selected slot, no          */
/* confirmation (see fa_erase_selected()), same as the keyboard Delete     */
/* key. Move arms MOVE MODE for the selected favorite -- same FLAT_HAND/   */
/* row-click flow as PLACE MODE, just swapping an existing favorite        */
/* (fa_move_source_slot) with the destination instead of placing a browser */
/* filename. Start shows the same "Now starting <name>"                    */
/* stub as double-click/Enter on the selected favorite (fa_start_selected()) */
/* -- same "several ways to confirm" idiom the browser's own [Add]/        */
/* double-click/Enter already use. Browser opens the nested browser (see   */
/* above); Source opens the server/source selector (fa_open_source()) --   */
/* this used to be the browser's own button, moved here since which mount  */
/* is active determines which favorites are shown just as much as which    */
/* files the browser lists. Quit is the only button that actually ends     */
/* this loop, exiting the whole program (dialog_run() returning to         */
/* main.c's own appl_exit()).                                              */
/* ================================================================== */
enum {
    FA_ROOT = 0,
    FA_TITLE,
    FA_DIV1,
    FA_TAB_BASE
};
#define FA_TAB_COUNT   4
#define FA_TAB(i)      (FA_TAB_BASE + (i))
#define FA_AFTER_TABS  (FA_TAB_BASE + FA_TAB_COUNT)
#define FA_DIV2        (FA_AFTER_TABS + 0)
#define FA_ROW_BASE    (FA_AFTER_TABS + 1)
/* One favorites page = 15 slots, matching the task's own [01-15] mockup
 * (and, not coincidentally, the browser's own one-screen page size --
 * FA_ROWS is kept as its own independent constant rather than reusing
 * FM_MAX_VISIBLE_FILES, defined further down, to keep this section
 * self-contained). */
#define FA_ROWS        15
#define FA_ROW(i)      (FA_ROW_BASE + (i))
#define FA_AFTER_ROWS  (FA_ROW_BASE + FA_ROWS)
#define FA_DIV3        (FA_AFTER_ROWS + 0)
/* Leftmost, same position [Source] used to have in the browser -- moved
 * here per this task's own request (the browser's own former
 * FM_SOURCE_BTN). Move/Erase/Start/Browser shifted right to make room. */
#define FA_SOURCE_BTN  (FA_AFTER_ROWS + 1)
#define FA_MOVE_BTN    (FA_AFTER_ROWS + 2)
#define FA_ERASE_BTN   (FA_AFTER_ROWS + 3) /* empties the selected slot -- "Erase", not "Delete": nothing is thrown away, a slot just becomes free again */
#define FA_START_BTN   (FA_AFTER_ROWS + 4)
#define FA_BROWSER_BTN (FA_AFTER_ROWS + 5)
/* Favorites is now the app's own top-level screen (dialog_run() runs ITS
 * loop directly, opening the browser as a nested call on [Browser] --
 * see dialog_run()'s own comment) -- so the program's actual Quit lives
 * here now, not in the browser (which has a plain [Back] instead, see
 * FM_BACK_BTN). Right-anchored from DW, same convention FM_PREV_BTN/
 * FM_NEXT_BTN already use for right-aligned buttons. */
#define FA_QUIT_BTN    (FA_AFTER_ROWS + 6)
#define FA_NOBJS       (FA_AFTER_ROWS + 7)
static OBJECT fa_dlg[FA_NOBJS];

#define FA_TITLE_BUF 48
static char fa_title_text[FA_TITLE_BUF];
#define FA_SOURCE_BTN_BUF 16
static char fa_source_btn_text[FA_SOURCE_BTN_BUF]; /* "Source: TNFS" / "Source: SD" -- own text doubles as the value, same idiom fm_source_btn_text used before this button moved here */
/* Content width matches the row object's own real width (STD_FRAME_CHARS,
 * same derivation as FM_CONTENT_CHARS: dialog width minus a 1-char margin
 * each side) -- fa_refresh_rows() always pads its text out to exactly this
 * many characters, see that function's own comment for why. */
#define FA_CONTENT_CHARS (STD_FRAME_CHARS - 2)
#define FA_ROW_BUF (FA_CONTENT_CHARS + 1)
static char fa_row_text[FA_ROWS][FA_ROW_BUF];
static int fa_current_page; /* 0..FA_TAB_COUNT-1, which tab is showing -- mock data only, see fa_refresh_rows() */
static int fa_short_screen; /* set by fa_dialog_init(), read by dialog_run() -- same role as fm_short_screen */
static int fa_selected_row = -1; /* -1 = no favorite selected on the current page -- same role as fm_selected_row */

static void fa_dialog_init(void)
{
    LayoutMetrics lm;
    StdFrame f;
    int tab_w, tab_gap;
    int i;

    layout_metrics_get(&lm);
    /* std_frame_get() with the SAME row_count (FA_ROWS == FM_MAX_VISIBLE_FILES,
     * both 15) reproduces the main browser's own frame geometry exactly --
     * see this dialog's own header comment on why the two windows must be
     * pixel-identical in size, and std_frame_get()'s own comment for the
     * shared formula. The main window's own fm_dialog_init() is untouched
     * and does not call this helper; std_frame_get() simply mirrors its
     * math independently so fm_dialog_init() never has to change for this
     * dialog to match it. */
    std_frame_get(&f, &lm, FA_ROWS);
    fa_short_screen = f.short_screen;

    set_obj(fa_dlg, FA_ROOT, G_BOX, NONE, NORMAL, 0, 0, f.DW, f.DH);
    fa_dlg[FA_ROOT].ob_spec.index = f.root_spec;

    set_obj(fa_dlg, FA_TITLE, G_STRING, NONE, NORMAL, f.xl, f.yt, f.DW - 2*f.xl, lm.rh);
    fa_dlg[FA_TITLE].ob_spec.free_string = fa_title_text;

    /* FA_DIV1 (title/tabs separator) and FA_DIV3 (rows/buttons separator)
     * are HIDETREE'd on a short screen, exactly like FM_DIV1/FM_DIV3 --
     * see fm_dialog_init()'s own comment for why (still too tall at 15
     * rows with every line in place; FA_DIV2 stays either way). Always
     * set_obj()'d regardless, same real-hardware-crash reason as FM_DIV1/3. */
    set_obj(fa_dlg, FA_DIV1, G_BOX, NONE, NORMAL, f.xl, f.ydiv1, f.DW - 2*f.xl, 2);
    fa_dlg[FA_DIV1].ob_spec.index = 0x00001171L;
    if (f.short_screen)
        fa_dlg[FA_DIV1].ob_flags |= (unsigned short)HIDETREE;

    tab_w = 12 * lm.cw;
    tab_gap = lm.cw;
    for (i = 0; i < FA_TAB_COUNT; i++) {
        int tx = f.xl + i * (tab_w + tab_gap);
        set_obj(fa_dlg, FA_TAB(i), G_BUTTON, EXIT | TOUCHEXIT, NORMAL, tx, f.ysecond, tab_w, lm.rh);
    }
    fa_dlg[FA_TAB(0)].ob_spec.free_string = " 01-15 ";
    fa_dlg[FA_TAB(1)].ob_spec.free_string = " 16-30 ";
    fa_dlg[FA_TAB(2)].ob_spec.free_string = " 31-45 ";
    fa_dlg[FA_TAB(3)].ob_spec.free_string = " 46-60 ";

    set_obj(fa_dlg, FA_DIV2, G_BOX, NONE, NORMAL, f.xl, f.ydiv2, f.DW - 2*f.xl, 2);
    fa_dlg[FA_DIV2].ob_spec.index = 0x00001171L;

    for (i = 0; i < FA_ROWS; i++) {
        int ry = f.yrow0 + i * lm.pitch;
        /* SELECTABLE | RBUTTON, same family FM_ROW uses -- selectable with
         * a highlight, single-selection within the page (RBUTTON auto-
         * deselects whichever row was selected before). NOT EXIT: a click
         * only selects, form_button() reports it back to the caller
         * instead of ending the dialog -- see dialog_run()'s own row
         * handling below for what a single vs. double click then does. */
        set_obj(fa_dlg, FA_ROW(i), G_STRING, SELECTABLE | RBUTTON, NORMAL, f.xl, ry, f.DW - 2*f.xl, lm.rh);
        fa_dlg[FA_ROW(i)].ob_spec.free_string = fa_row_text[i];
    }

    set_obj(fa_dlg, FA_DIV3, G_BOX, NONE, NORMAL, f.xl, f.ydiv3, f.DW - 2*f.xl, 2);
    fa_dlg[FA_DIV3].ob_spec.index = 0x00001171L;
    if (f.short_screen)
        fa_dlg[FA_DIV3].ob_flags |= (unsigned short)HIDETREE;

    /* Leftmost -- same position/width [Source] had in the browser (see
     * FA_SOURCE_BTN's own enum comment). Move/Erase/Start/Browser shifted
     * right by that same 13cw + 1cw gap so their own spacing is unchanged. */
    set_obj(fa_dlg, FA_SOURCE_BTN, G_BUTTON, EXIT | TOUCHEXIT, NORMAL, f.xl, f.ybtn, 13*lm.cw, lm.rh);
    fa_dlg[FA_SOURCE_BTN].ob_spec.free_string = fa_source_btn_text;

    set_obj(fa_dlg, FA_MOVE_BTN, G_BUTTON, EXIT | TOUCHEXIT, NORMAL, f.xl + 14*lm.cw, f.ybtn, 11*lm.cw, lm.rh);
    fa_dlg[FA_MOVE_BTN].ob_spec.free_string = "  Move  ";

    set_obj(fa_dlg, FA_ERASE_BTN, G_BUTTON, EXIT | TOUCHEXIT, NORMAL, f.xl + 26*lm.cw, f.ybtn, 11*lm.cw, lm.rh);
    fa_dlg[FA_ERASE_BTN].ob_spec.free_string = " Erase  ";

    set_obj(fa_dlg, FA_START_BTN, G_BUTTON, EXIT | TOUCHEXIT, NORMAL, f.xl + 38*lm.cw, f.ybtn, 11*lm.cw, lm.rh);
    fa_dlg[FA_START_BTN].ob_spec.free_string = " Start  ";

    set_obj(fa_dlg, FA_BROWSER_BTN, G_BUTTON, EXIT | TOUCHEXIT, NORMAL, f.xl + 50*lm.cw, f.ybtn, 13*lm.cw, lm.rh);
    fa_dlg[FA_BROWSER_BTN].ob_spec.free_string = "  Browser  ";

    {
        int quit_w = 8 * lm.cw;
        set_obj(fa_dlg, FA_QUIT_BTN, G_BUTTON, EXIT | TOUCHEXIT, NORMAL, f.DW - f.xl - quit_w, f.ybtn, quit_w, lm.rh);
        fa_dlg[FA_QUIT_BTN].ob_spec.free_string = " Quit  ";
    }

    wire_tree(fa_dlg, FA_NOBJS);
}

/* Favorites are no longer held in memory (see src/favcfg.c) -- every read
 * or write below goes straight to that mount's own MOUNTn.CFG on disk, one
 * favorite (or one visible page's worth of names) at a time. FA_TOTAL_SLOTS
 * remains just the UI's own page/row bound (60 = FA_TAB_COUNT pages of
 * FA_ROWS each); favcfg.c has its own identical constant for the same 60,
 * kept private to that file since it never needs to be UI-shaped. */
#define FA_TOTAL_SLOTS (FA_ROWS * FA_TAB_COUNT)

/* Set once, at the top of dialog_run(), to that same call's own cfg
 * pointer -- lets every FA_* helper below reach the CURRENT mount slot
 * (cfg->active_index can change any time the user picks a different
 * source, see server_selector_run()) without threading a cfg/mount_slot
 * parameter through each of them individually. Safe: cfg outlives
 * dialog_run() itself, so the pointer stays valid for as long as this
 * static does. */
static ProfileConfig *fa_cfg;

/* The mount slot (1..8) favcfg.c's own functions expect, derived from
 * fa_cfg->active_index -- or 0 (a deliberately out-of-range "no mount"
 * value every favcfg_*() function already treats as a safe no-op/empty
 * read) if nothing is configured/active yet. */
static int fa_mount_slot(void)
{
    if (!fa_cfg || fa_cfg->active_index < 0 || fa_cfg->active_index >= MAX_PROFILES)
        return 0;
    return fa_cfg->active_index + 1;
}

/* G_STRING objects have no fill of their own -- objc_draw() only paints
 * glyphs on top of whatever is already on screen (same issue this file's
 * own arrow-key row-highlight code ran into once already, see
 * fm_form_do_events()'s comment on that). Without fixed-width padding,
 * placing a short name over "<empty>" (or a longer name) left stray
 * glyphs from the old text showing through around the new one -- always
 * padding out to the row's own full FA_CONTENT_CHARS width, right-padded
 * with spaces via "%-Ns.Ns", guarantees every redraw fully overwrites
 * whatever was there before, regardless of old vs. new length.
 *
 * Also (re)marks which of the four tab buttons is the active page,
 * SELECTED (drawn inverted, like a pressed button -- the same STATE bit
 * FA_ROW/FM_ROW use for their own highlight, it applies to G_BUTTON just
 * as well regardless of that object also being EXIT|TOUCHEXIT) so the
 * active tab stays visibly marked rather than only flashing during its
 * own click. Called on every page-content change (initial open, tab
 * switch, return from Browser), so this always stays in sync with
 * fa_current_page without a separate call site of its own. */
static void fa_refresh_rows(void)
{
    int i;
    int base = fa_current_page * FA_ROWS; /* slot NUMBERING must follow the active tab -- page 1 (16-30) shows 16..30, not 01..15 again */
    int mount_slot = fa_mount_slot();
    char names[FA_ROWS][FAVCFG_NAME_LEN];

    /* One open/read of MOUNTn.CFG for the whole page, not FA_ROWS (15)
     * separate ones -- see favcfg_read_page_names()'s own comment. On a
     * TNFS-backed drive each separate file open has its own real network
     * cost, which is what made building a page of favorites visibly slow
     * (and left the screen blank for a moment right after startup, since
     * this runs before the dialog's first dialog_open()). */
    favcfg_read_page_names(mount_slot, base + 1, FA_ROWS, names);

    for (i = 0; i < FA_ROWS; i++) {
        int slot = base + i;
        const char *name = names[i][0] != '\0' ? names[i] : "<empty>";
        sprintf(fa_row_text[i], "%02d  %-*.*s", slot + 1,
                FA_CONTENT_CHARS - 4, FA_CONTENT_CHARS - 4, name);
        fa_dlg[FA_ROW(i)].ob_state &= (unsigned short)(~SELECTED);
    }
    fa_selected_row = -1; /* whatever was selected belonged to the old content/page -- same reset fm_apply_entries_to_rows() does */

    for (i = 0; i < FA_TAB_COUNT; i++) {
        if (i == fa_current_page)
            fa_dlg[FA_TAB(i)].ob_state |= (unsigned short)SELECTED;
        else
            fa_dlg[FA_TAB(i)].ob_state &= (unsigned short)(~SELECTED);
    }
}

/* PLACE MODE and MOVE MODE share the same FLAT_HAND hover cursor and
 * "click a row to act" flow (see dialog_run()'s own MU_M1/MU_BUTTON
 * handling below) -- they only differ in the source of the name being
 * placed: a browser filename (fa_place_name) for PLACE, an existing
 * favorite slot (fa_move_source_slot) for MOVE. */
typedef enum {
    FAVORITES_MODE_NORMAL = 0,
    FAVORITES_MODE_PLACE,
    FAVORITES_MODE_MOVE
} FavoritesMode;
static FavoritesMode fa_mode;
static char fa_place_name[FLOPPY_BROWSE_NAME_LEN]; /* the browser filename waiting to be placed -- valid only while fa_mode == FAVORITES_MODE_PLACE */
static char fa_place_dir[FLOPPY_BROWSE_CWD_LEN]; /* the browser's own current directory at the time fa_place_name was captured -- stored alongside it, same lifetime */
static int fa_move_source_slot = -1; /* absolute slot (0..FA_TOTAL_SLOTS-1) being moved -- valid only while fa_mode == FAVORITES_MODE_MOVE */

/* Maps an absolute screen point to a favorite ROW index (0..FA_ROWS-1) on
 * the CURRENTLY displayed page, or -1 if outside every row. Same
 * root-is-absolute-after-dialog_open()/children-are-relative technique
 * fm_row_near() already relies on for the main browser's own rows. The
 * caller adds fa_current_page*FA_ROWS to get the absolute favorite number
 * (1..60) -- see this task's own "IMPORTANT: CURRENT SERVER / PAGE"
 * requirement. */
static int fa_row_at(short mx, short my)
{
    int i;
    short root_x = fa_dlg[FA_ROOT].ob_x;
    short root_y = fa_dlg[FA_ROOT].ob_y;

    for (i = 0; i < FA_ROWS; i++) {
        OBJECT *row = &fa_dlg[FA_ROW(i)];
        short x = (short)(root_x + row->ob_x);
        short y = (short)(root_y + row->ob_y);

        if (mx < x || mx >= x + row->ob_width)
            continue;
        if (my < y || my >= y + row->ob_height)
            continue;
        return i;
    }
    return -1;
}

/* Redraws ONE row without repainting the other 14 (unlike a tab switch,
 * where every row's content changes anyway) -- placing a favorite only
 * ever changes the one row that was clicked. G_STRING objects paint text
 * ink only, in transparent mode, never a background (see fa_refresh_rows()'s
 * own comment) -- so unlike redrawing FA_ROOT (whose G_BOX fill repaints
 * white for every child), a single row needs its own small rectangle
 * erased first via a plain VDI v_bar(), then just that row's objc_draw()
 * on top. graf_handle()'s return value (ignored everywhere else in this
 * file, which only ever needs its cw/ch/bw/bh OUT params) is the VDI
 * workstation handle AES itself already uses -- reused here rather than
 * opening a second one, and left with no cleanup/restore afterward since
 * every AES draw call (including the objc_draw() below) sets up whatever
 * VDI text/fill attributes IT needs itself rather than trusting leftover
 * state, the same assumption this codebase already relies on implicitly
 * everywhere else. */
static void fa_redraw_row(int row)
{
    short cw, ch, bw, bh;
    short handle;
    short pxy[4];
    OBJECT *ro = &fa_dlg[FA_ROW(row)];
    short x = (short)(fa_dlg[FA_ROOT].ob_x + ro->ob_x);
    short y = (short)(fa_dlg[FA_ROOT].ob_y + ro->ob_y);

    handle = graf_handle(&cw, &ch, &bw, &bh);

    pxy[0] = x;
    pxy[1] = y;
    pxy[2] = (short)(x + ro->ob_width - 1);
    pxy[3] = (short)(y + ro->ob_height - 1);

    vswr_mode(handle, MD_REPLACE);
    vsf_interior(handle, FIS_SOLID);
    vsf_color(handle, WHITE);
    v_bar(handle, pxy);

    objc_draw(fa_dlg, FA_ROW(row), MAX_DEPTH, x, y, ro->ob_width, ro->ob_height);
}

/* Shows a "Now starting <name>" stub alert for the currently selected
 * favorite -- same stub fm_start_selected() used to show in the browser
 * before Step 4 replaced it with opening this dialog; still no real
 * mount/emulation behind it. Shared by a double-click on a row and
 * Return/Enter (dialog_run()'s own MU_KEYBD handling below), same "two
 * ways to confirm a selection" idea fm_form_do_events() already
 * established for the browser. No-op if nothing is selected, or the
 * selected slot is empty -- there is nothing to start either way. */
static void fa_start_selected(void)
{
    int slot, dir_len;
    char name[FAVCFG_NAME_LEN], dir[FAVCFG_DIR_LEN];
    char path[FAVCFG_DIR_LEN + FAVCFG_NAME_LEN];
    char msg[100];

    if (fa_selected_row < 0)
        return;
    slot = fa_current_page * FA_ROWS + fa_selected_row;
    favcfg_read_entry(fa_mount_slot(), slot + 1, name, sizeof(name), dir, sizeof(dir));
    if (name[0] == '\0')
        return;

    /* Joined with a forward slash, matching the browse protocol's own
     * path convention (dir is always "/..."-style, e.g. "/GAMES", never
     * backslash -- see this function's own history for what went wrong
     * with "\\" here before) -- and no doubled "//" when dir is already
     * "/" (root) or otherwise ends with one. */
    dir_len = (int)strlen(dir);
    if (dir_len > 0 && dir[dir_len - 1] == '/')
        sprintf(path, "%s%s", dir, name);
    else
        sprintf(path, "%s/%s", dir, name);

    /* The JOINED path, truncated to the same total length this alert
     * already used safely for a single field before -- a longer combined
     * dir+name line reproducibly corrupted the alert's own OK button on
     * real TOS 1.x/2.x hardware (see this function's own history), so
     * the truncation budget stays the same regardless of which string
     * fills it. */
    sprintf(msg, "[1][Now starting:|%-.60s][OK]", path);
    form_alert(1, msg);
}

/* Empties the currently selected favorite slot -- "Erase", not "Delete":
 * nothing is thrown away, a slot just becomes free again, so no
 * confirmation prompt either (per this task's own brief). Shared by
 * [Erase] (FA_ERASE_BTN) and the keyboard Delete key (dialog_run()'s own
 * MU_KEYBD handling below). No-op if nothing is selected -- there is
 * nothing to erase either way; an already-empty slot erases to the same
 * empty state harmlessly if somehow selected. */
static void fa_erase_selected(void)
{
    int slot, row;

    if (fa_selected_row < 0)
        return;
    row = fa_selected_row;
    slot = fa_current_page * FA_ROWS + row;
    favcfg_erase_entry(fa_mount_slot(), slot + 1);
    fa_refresh_rows();
    fa_redraw_row(row); /* only this one row changed -- same reasoning as the PLACE MODE placement code */
}

/* Arms MOVE MODE for the currently selected favorite -- shared by [Move]
 * (FA_MOVE_BTN, dialog_run()'s own MU_BUTTON handling) and the M key
 * (dialog_run()'s own MU_KEYBD handling), NORMAL MODE only for the key
 * (see that call site's own comment). No-op (returns hand_shown
 * unchanged) if nothing is selected. Returns the new hand_shown value --
 * the caller must assign it to its own local, since the ongoing MU_M1
 * hover watch in dialog_run()'s main loop depends on it (this function
 * cannot update a caller's local by itself). row_x/y/w/h are recomputed
 * here rather than passed in: cheap (a handful of integer ops against
 * FA_ROOT/FA_ROW(0)'s own already-set geometry, same values dialog_run()
 * itself computes once at startup and never changes), and keeps this
 * function's own signature to just the one value that genuinely differs
 * per call. */
static int fa_arm_move(int hand_shown)
{
    int row;
    short mx, my, mb, ks;
    short row_x, row_y, row_w, row_h;

    if (fa_selected_row < 0)
        return hand_shown;

    row = fa_selected_row;
    fa_move_source_slot = fa_current_page * FA_ROWS + row;
    fa_mode = FAVORITES_MODE_MOVE;

    /* MOVE MODE has no "selected row" concept, only a pending source slot
     * -- clear the row's own highlight, same as PLACE MODE never leaves a
     * row selected either. */
    fa_dlg[FA_ROW(row)].ob_state &= (unsigned short)(~SELECTED);
    fa_selected_row = -1;
    fa_redraw_row(row);

    row_x = (short)(fa_dlg[FA_ROOT].ob_x + fa_dlg[FA_ROW(0)].ob_x);
    row_y = (short)(fa_dlg[FA_ROOT].ob_y + fa_dlg[FA_ROW(0)].ob_y);
    row_w = fa_dlg[FA_ROW(0)].ob_width;
    row_h = (short)((fa_dlg[FA_ROOT].ob_y + fa_dlg[FA_ROW(FA_ROWS - 1)].ob_y
                      + fa_dlg[FA_ROW(FA_ROWS - 1)].ob_height) - row_y);

    /* Cursor from the mouse's ACTUAL current position, same reasoning as
     * FA_BROWSER_BTN's own post-Browser cursor recompute. */
    graf_mkstate(&mx, &my, &mb, &ks);
    hand_shown = (mx >= row_x && mx < row_x + row_w && my >= row_y && my < row_y + row_h);
    graf_mouse(hand_shown ? FLAT_HAND : ARROW, 0L);

    return hand_shown;
}

/* Switches to page (0..FA_TAB_COUNT-1) and redraws -- shared by a tab
 * click (FA_TAB(page), dialog_run()'s own MU_BUTTON handling) and the
 * F1-F4 keys (dialog_run()'s own MU_KEYBD handling). Mode-independent
 * (callable during PLACE/MOVE MODE too, same as the mouse tab click
 * already was) -- fa_refresh_rows() itself resets any row selection and
 * re-marks the new active tab SELECTED. */
static void fa_switch_page(int page)
{
    fa_current_page = page;
    fa_refresh_rows();
    objc_draw(fa_dlg, FA_ROOT, MAX_DEPTH, fa_dlg[FA_ROOT].ob_x, fa_dlg[FA_ROOT].ob_y,
              fa_dlg[FA_ROOT].ob_width, fa_dlg[FA_ROOT].ob_height);
}

/* Deselects the currently selected favorite, if any -- called on Esc
 * (dialog_run()'s own MU_KEYBD handling below), NORMAL MODE only (see
 * that call site's own comment on why). Right-click deselection was
 * tried twice (an undocumented evnt_multi() bclicks bit, then a
 * documented MU_TIMER + graf_mkstate() poll) and dropped: the first broke
 * ordinary clicks outright, the second worked but felt unreliable/laggy
 * in practice -- Esc alone is the current, deliberately simpler design.
 *
 * NOT form_button() re-clicked on the same row -- see fm_deselect()'s own
 * comment for why that never worked (FA_ROW, like FM_ROW, is SELECTABLE |
 * RBUTTON; EmuTOS's own fm_button() always re-selects an RBUTTON member
 * that is clicked again, never toggles it off). Clears the bit directly
 * and uses fa_redraw_row()'s own erase-then-draw instead, exactly like
 * PLACE MODE placement already does when a row's content changes. */
static void fa_deselect(void)
{
    int row;

    if (fa_selected_row < 0)
        return;
    row = fa_selected_row;
    fa_dlg[FA_ROW(row)].ob_state &= (unsigned short)(~SELECTED);
    fa_selected_row = -1;
    fa_redraw_row(row);
}

static const char *profile_backend_word(const Profile *p); /* defined below -- see that definition's own comment */

/* Computes the FA_TITLE and FA_SOURCE_BTN text from cfg's own active
 * profile -- shared by dialog_run()'s own startup and by its post-Browser/
 * post-Source refresh (the active source can change via either [Browser]'s
 * own nested [Source] history or FA_SOURCE_BTN itself), per this task's
 * own "Favorites - <server nickname>" requirement. */
static void fa_update_title(const ProfileConfig *cfg)
{
    int have_active = (cfg->active_index >= 0 && cfg->active_index < MAX_PROFILES
                        && profile_slot_is_configured(&cfg->profiles[cfg->active_index]));

    if (have_active) {
        sprintf(fa_title_text, "Favorites - %-.30s", cfg->profiles[cfg->active_index].nickname);
        sprintf(fa_source_btn_text, "Source: %s", profile_backend_word(&cfg->profiles[cfg->active_index]));
    } else {
        sprintf(fa_title_text, "Favorites - (no source)");
        strncpy(fa_source_btn_text, "Source", sizeof(fa_source_btn_text) - 1);
        fa_source_btn_text[sizeof(fa_source_btn_text) - 1] = '\0';
    }
}

/* 16, not the LFN browser's own 25-per-page figure (RESEARCH-STEP0.md) --
 * 25 rows plus the title/source/directory lines and buttons made the main
 * window taller than a real 640x200 medium-resolution screen (~223px
 * needed vs. 200px available); 20 rows (~188px) still fit but left little
 * margin. 16 rows gives comfortable headroom on medium resolution. If a
 * real 25-entry page is needed later, this will need either a scrollable
 * list or a resolution-dependent row count, not just raising this
 * constant back up. Defined here (not down in the FM_* section below, its
 * logical home) because the real browse-entry state right below already
 * needs it. */
#define FM_MAX_VISIBLE_FILES 15

/* ================================================================== */
/* Real directory browser state (Step 2)                               */
/*                                                                       */
/* One active browse session (fp_gen/fp_browse_ok below), mirroring the  */
/* single-session contract sidetnfs_floppy_browse.h already documents on */
/* the firmware side -- FLOPPY.PRG is one Atari-side client, so there is */
/* never more than one open profile/CWD to track here either. No         */
/* pagination UI yet (Step 3): every list below is always page 0 of the  */
/* active CWD, capped at FM_MAX_VISIBLE_FILES entries. Directories and   */
/* files come back from the firmware as two SEPARATE pages (never mixed  */
/* in one page, see GEMDRVEMUL_FLOPPY_PAGE) -- fm_load_entries() below   */
/* combines them for display: directories first, then files, both drawn */
/* from the very start of their own respective page-0 listing. */
/* ================================================================== */
static unsigned long fm_gen;     /* current browse generation; 0 = no active session */
static int fm_browse_ok;         /* 1 once BROWSE_OPEN has succeeded for fm_gen */
static int fm_browse_profile_index = -1; /* which cfg->profiles[] slot fm_gen belongs to, -1 = none */
static char fm_cwd[FLOPPY_BROWSE_CWD_LEN]; /* current real CWD, full 256 bytes (fm_title_text is a truncated display copy) */

typedef struct {
    char name[FLOPPY_BROWSE_NAME_LEN]; /* full name, NOT truncated -- CHANGE_DIR needs the exact name */
    int is_dir;
} FmEntry;
static FmEntry fm_entries[FM_MAX_VISIBLE_FILES];
static int fm_entry_count;
static unsigned long fm_page_index; /* current GET_PAGE page, reset to 0 on open/navigate */
static int fm_has_prev, fm_has_next; /* from the last fm_load_entries() call -- drives FM_PREV_BTN/FM_NEXT_BTN's enabled state */

/* ================================================================== */
/* Profile source/path helpers                                         */
/* ================================================================== */

static const char *profile_backend_word(const Profile *p)
{
    return (p->backend == PROFILE_BACKEND_SD) ? "SD" : "TNFS";
}

/* The profile's own configured root -- mount_path for TNFS, sd_path for
 * SD. FM_DIRUP_BTN's handler never lets the user navigate above this. */
static void profile_root_dir(const Profile *p, char *out, int outsize)
{
    const char *root = (p->backend == PROFILE_BACKEND_SD) ? p->sd_path : p->mount_path;
    strncpy(out, root, outsize - 1);
    out[outsize - 1] = '\0';
}

/* Opens (or re-uses) the real browse session for cfg->active_index --
 * BROWSE_OPEN is only actually sent when switching to a DIFFERENT profile
 * than the one fm_gen already belongs to, so a routine refresh never
 * silently resets navigation the user already did via CHANGE_DIR back to
 * last_directory. On success, updates fm_gen/fm_cwd and persists the
 * resulting CWD into the profile's own last_directory (RAM only -- an
 * explicit Save is still required to reach flash, same policy as before).
 * Returns 1 on success, 0 on failure (already alerted via form_alert). */
static int fm_open_browse_for_active_profile(ProfileConfig *cfg)
{
    FloppyBrowseResult r;
    int rc;

    if (fm_browse_ok && fm_browse_profile_index == cfg->active_index)
        return 1;

    rc = floppy_probe_browse_open((unsigned long)cfg->active_index, &r);
    if (rc != FLOPPY_PROBE_OK) {
        fm_browse_ok = 0;
        form_alert(1, "[3][Could not reach the|cartridge (timeout).][OK]");
        return 0;
    }
    if (r.status != FLOPPY_BROWSE_OK) {
        char msg[96];
        fm_browse_ok = 0;
        sprintf(msg, "[3][Could not open this source|(error %lu).][OK]", r.status);
        form_alert(1, msg);
        return 0;
    }

    fm_gen = r.generation;
    fm_browse_profile_index = cfg->active_index;
    fm_browse_ok = 1;
    strncpy(fm_cwd, r.cwd, sizeof(fm_cwd) - 1);
    fm_cwd[sizeof(fm_cwd) - 1] = '\0';
    strncpy(cfg->profiles[cfg->active_index].last_directory, fm_cwd, PROFILE_LASTDIR_LEN - 1);
    cfg->profiles[cfg->active_index].last_directory[PROFILE_LASTDIR_LEN - 1] = '\0';
    return 1;
}

/* Fills fm_entries[]/fm_entry_count from page fm_page_index of the active
 * CWD's combined dirs-then-files listing -- the firmware returns one page
 * (dirs always sorted before files within it, see FloppyPageResult's own
 * comment), one round trip, up to FLOPPY_BROWSE_PAGE_ENTRIES entries
 * (which is exactly FM_MAX_VISIBLE_FILES -- one firmware page IS one
 * screen). Also refreshes fm_has_prev/fm_has_next from the page result --
 * callers that change fm_page_index (Prev/Next) or reset it to 0 (open,
 * navigate) must call this again afterward, same as they already do for
 * fm_entries/fm_entry_count. A FLOPPY_PROBE_OK communication result with a
 * non-OK/non-END_OF_DIRECTORY browse status is alerted once; count is
 * always trustworthy as 0 in every error case
 * (sidetnfs_floppy_browse_get_page()'s own contract), so the list is
 * simply left empty rather than guessed at. */
static void fm_load_entries(void)
{
    FloppyPageResult p;
    int i, n;

    fm_entry_count = 0;
    fm_has_prev = fm_has_next = 0;
    if (!fm_browse_ok)
        return;

    if (floppy_probe_browse_get_page(fm_gen, fm_page_index, &p) != FLOPPY_PROBE_OK) {
        form_alert(1, "[3][Could not reach the|cartridge (timeout).][OK]");
        return;
    }
    if (p.status != FLOPPY_BROWSE_OK && p.status != FLOPPY_BROWSE_STATUS_END_OF_DIRECTORY) {
        char msg[96];
        sprintf(msg, "[3][Could not list directory|(error %lu).][OK]", p.status);
        form_alert(1, msg);
        return;
    }
    fm_has_prev = p.has_prev ? 1 : 0;
    fm_has_next = p.has_next ? 1 : 0;
    n = (int)p.count;
    if (n > FM_MAX_VISIBLE_FILES) n = FM_MAX_VISIBLE_FILES;
    for (i = 0; i < n; i++) {
        strncpy(fm_entries[i].name, p.entries[i], FLOPPY_BROWSE_NAME_LEN - 1);
        fm_entries[i].name[FLOPPY_BROWSE_NAME_LEN - 1] = '\0';
        fm_entries[i].is_dir = p.is_dir[i];
    }
    fm_entry_count = n;
}

/* Navigates the active browse session: go_up=1 for ".." (name ignored),
 * else descends into `name` (must be one of the CURRENT CWD's own
 * directory-page entries). Updates fm_gen/fm_cwd and persists the
 * resulting CWD into the active profile's last_directory (RAM only) on
 * success. Returns 1 on success, 0 on failure (already alerted) -- on
 * failure fm_gen/fm_cwd are left exactly as they were, matching
 * FLOPPY_BROWSE_CHANGE_DIR's own "a failed change never moves the CWD"
 * contract. Caller is responsible for calling fm_load_entries() again
 * afterward. */
static int fm_change_dir(ProfileConfig *cfg, int go_up, const char *name)
{
    FloppyBrowseResult r;
    int rc;

    rc = floppy_probe_browse_change_dir(fm_gen, go_up, name ? name : "", &r);
    if (rc != FLOPPY_PROBE_OK) {
        form_alert(1, "[3][Could not reach the|cartridge (timeout).][OK]");
        return 0;
    }
    if (r.status != FLOPPY_BROWSE_OK) {
        char msg[96];
        sprintf(msg, "[3][Could not change directory|(error %lu).][OK]", r.status);
        form_alert(1, msg);
        return 0;
    }

    fm_gen = r.generation;
    strncpy(fm_cwd, r.cwd, sizeof(fm_cwd) - 1);
    fm_cwd[sizeof(fm_cwd) - 1] = '\0';
    strncpy(cfg->profiles[cfg->active_index].last_directory, fm_cwd, PROFILE_LASTDIR_LEN - 1);
    cfg->profiles[cfg->active_index].last_directory[PROFILE_LASTDIR_LEN - 1] = '\0';
    return 1;
}

/* ================================================================== */
/* Main window (FM_*)                                                  */
/* The central hub: shown immediately at startup (see dialog_run()) and  */
/* returned to after every Source/Dir Up/Start action -- never an        */
/* intermediate "pick a source first" gate. Source opens the already-    */
/* existing selector/editor dialog directly; directories are navigated   */
/* inline in the row list itself (double-click to descend, Dir Up to go  */
/* up a level) -- there is no separate directory-picker dialog anymore.  */
/* ================================================================== */
enum {
    FM_ROOT = 0,
    FM_TITLE,
    FM_DIV1,
    FM_SOURCE_LINE,
    /* No separate "Directory: X" line here -- FM_TITLE already IS the
     * current directory, so this would only ever duplicate it (and, when
     * empty/no source selected, left a blank row that pushed the buttons
     * partly off-screen on real hardware). */
    FM_DIV2,
    FM_ROW_BASE
};
#define FM_ROW(i)      (FM_ROW_BASE + (i))
#define FM_AFTER_ROWS  (FM_ROW_BASE + FM_MAX_VISIBLE_FILES)
#define FM_DIV3          (FM_AFTER_ROWS + 0)
/* [Source] used to live here -- moved to Favorites (FA_SOURCE_BTN), same
 * position (leftmost of its button row), per this task's own request.
 * DirUp/Add/Back shifted left to fill the gap it left behind. */
#define FM_DIRUP_BTN     (FM_AFTER_ROWS + 1)
#define FM_ADD_BTN (FM_AFTER_ROWS + 2)
#define FM_BACK_BTN      (FM_AFTER_ROWS + 3) /* cancel back to Favorites -- see fm_browse_run()'s own comment. The program's actual Quit lives on Favorites now (FA_QUIT_BTN). */
/* Placeholder for pagination -- buttons only for now, no paging logic
 * wired in yet: GET_PAGE already supports an arbitrary page_index on the
 * protocol/firmware side (one combined dirs-then-files page per call, see
 * FloppyPageResult's own comment), but fm_load_entries() only ever asks
 * for page 0. Clicking either does nothing yet. */
#define FM_PREV_BTN      (FM_AFTER_ROWS + 4)
#define FM_NEXT_BTN      (FM_AFTER_ROWS + 5)
#define FM_NOBJS         (FM_AFTER_ROWS + 6)
static OBJECT fm_dlg[FM_NOBJS];

/* 78 characters wide -- the Atari ST's own medium/high resolution text
 * width (640px / 8px font = 80 chars), minus a 1-char margin each side --
 * per the task brief, so filenames can display up to 76 characters
 * (RESEARCH-STEP0.md's own "~80 chars, truncate cleanly" browser display
 * decision). This is now clearly the widest dialog in this application,
 * wider than SIDETNFS-Config's own 50-char widest dialogs. */
#define FM_DIALOG_CHARS 78
#define FM_CONTENT_CHARS 76 /* FM_DIALOG_CHARS minus the 1-char margin each side */

#define FM_TITLE_BUF  FM_DIALOG_CHARS
#define FM_SOURCE_BUF 48 /* "Source: " (8) + nickname (20) + "   Type: " (9) + backend word (up to 4) + NUL = 42 */
#define FM_ROW_BUF    (FM_CONTENT_CHARS + 1)      /* up to FM_CONTENT_CHARS of filename + NUL */
static char fm_title_text[FM_TITLE_BUF]; /* current directory (or a placeholder) -- see fm_refresh() */
static char fm_source_line[FM_SOURCE_BUF];
static char fm_row_text[FM_MAX_VISIBLE_FILES][FM_ROW_BUF];
static int fm_selected_row = -1; /* -1 = no file selected */
static int fm_short_screen; /* set by fm_dialog_init(), read by dialog_run() to open flush top-left instead of centered -- see dialog_open()'s own comment */

static void fm_dialog_init(void)
{
    LayoutMetrics lm;
    int DW, DH, xl;
    int yt, ydiv1, ysource, ydiv2, yrow0, ydiv3, ybtn;
    int i;

    layout_metrics_get(&lm);
    fm_short_screen = lm.short_screen;

    /* DW is the REAL measured screen width (lm.sw) on a medium-res-height
     * screen, not FM_DIALOG_CHARS*lm.cw -- that char-count estimate
     * assumed an 8px system font, which real hardware/Hatari testing
     * showed does not always hold (a visible gap of unused screen on the
     * right, confirmed by screenshot, not just a rounding sliver). xl is
     * the matching left/right inset -- 1px flush on a short screen (the
     * "no kaderrand" fullscreen look that was asked for), lm.cw otherwise
     * (unchanged from before). Every full-width object below (root,
     * dividers, title, source line, rows) uses DW/xl consistently so
     * nothing is ever narrower than the real screen on a short screen.
     * The two RIGHT-anchored buttons (Prev/Next, near the end of this
     * function) are positioned from DW-xl backward for the same reason:
     * their own on-screen position must never depend on a possibly-wrong
     * cw-based guess either. */
    DW = lm.short_screen ? lm.sw : (FM_DIALOG_CHARS * lm.cw);
    xl = lm.short_screen ? 1 : lm.cw;
    /* Top margin is also trimmed to a bare 1px on a medium-res-height
     * screen -- same "every pixel counts, we're right at the edge"
     * reasoning as the divider removal below, now paired with opening the
     * dialog flush against the screen's top-left corner instead of
     * centered (see dialog_open()'s own comment) so this margin is never
     * wasted doubled-up above AND below the visible content. */
    yt = lm.short_screen ? 1 : lm.tm;
    /* FM_DIV1 (title/source separator) and FM_DIV3 (rows/buttons
     * separator) are HIDETREE'd -- never left un-set_obj()'d, see below --
     * on a medium-res-height screen only: still too tall at 15 rows with
     * both lines in place. Each divider line itself is only 2px, but the
     * surrounding gaps it justified (5px above the row list, 7px below)
     * are what actually cost the space; collapsing straight to a tight
     * 2px gap in their place (still no visual overlap, just no line
     * drawn) is what actually buys back the few pixels needed. FM_DIV2
     * (source/row-list separator) stays either way -- removing it too
     * wasn't asked for and the rows/buttons gap alone already gets this
     * to fit. ydiv1/ydiv3 are still always computed (matching the
     * non-short layout) even when unused for the OTHER y-coordinates
     * below, purely so FM_DIV1/FM_DIV3 always get a valid set_obj() call
     * -- an object that's only ever HIDETREE'd and never otherwise
     * initialized is exactly the kind of half-set-up object that has
     * already crashed real hardware once this project (see the SD-folder
     * field / HIDETREE-overlap fixes) and must not happen again. */
    ydiv1   = yt + lm.rh + 1;
    ydiv3   = yt + lm.rh + 1; /* placeholder, overwritten below once yrow0 is known */
    if (lm.short_screen) {
        /* Measured precisely against lm.sh (the real screen work-area
         * height) via pixel-level inspection of real renders. The margin
         * this layout ends up with (a couple of pixels below the button
         * row, since setting DH to lm.sh exactly was tried and made
         * things worse for reasons not yet understood) is deliberately
         * spent as real breathing room between FM_DIV2 and the first row
         * (yrow0) instead of being wasted below the buttons -- the first
         * filename sitting flush against the divider line looked wrong,
         * and a sliver of desktop below the buttons is the lesser evil of
         * the two once some margin has to exist somewhere. */
        ysource = yt + lm.rh + 1;
        ydiv2   = ysource + lm.rh + 1;
        yrow0   = ydiv2 + 2 + 3; /* +2 for the divider line's own height, +3 real gap below it */
        ybtn    = yrow0 + FM_MAX_VISIBLE_FILES * lm.pitch + 2; /* +2 pushes the row down to where DH (below) lands exactly on lm.sh -- pixel inspection of the previous build (ybtn with no +2) showed a 2px sliver of desktop still visible below the buttons */
    } else {
        ysource = ydiv1 + 5;
        ydiv2   = ysource + lm.rh + 2;
        yrow0   = ydiv2 + 5;
        ybtn    = yrow0 + FM_MAX_VISIBLE_FILES * lm.pitch + 2 + 7;
    }
    ydiv3 = yrow0 + FM_MAX_VISIBLE_FILES * lm.pitch + 2; /* real value, valid in both branches */
    /* +1 below the button row is NOT optional padding -- pixel-level
     * inspection of a real render (Hatari screenshot) showed a G_BUTTON's
     * own bottom border is drawn one row past ob_y+ob_height, so trimming
     * this to exactly ybtn+rh (no slack at all) silently clipped every
     * button's bottom border across the whole row. Using lm.sh directly
     * here instead (to close the small gap below the buttons entirely)
     * was tried and made things WORSE on a real render -- more of the
     * desktop showed through, not less -- so this stays a computed sum,
     * not the raw measured screen height; the small gap below the button
     * row is the known-working state. */
    DH = lm.short_screen ? (ybtn + lm.rh + 1) : (ybtn + lm.rh + lm.tm + 3);

    set_obj(fm_dlg, FM_ROOT, G_BOX, NONE, NORMAL, 0, 0, DW, DH);
    /* 0x00031070L (every other dialog in this file) decodes, per this
     * toolchain's own BFOBSPEC bitfield layout (mt_gem.h: character:8,
     * framesize:8 SIGNED, framecol:4, textcol:4, textmode:1,
     * fillpattern:3, interiorcol:4, packed MSB-first), to framesize=3
     * (a 3px border), framecol=1 (black), fillpattern=7, interiorcol=0
     * (white) -- that visible black frame is exactly the "kaderrand" that
     * was asked to go away for a true fullscreen presentation. On a
     * medium-res-height screen only, framesize is cleared to 0
     * (0x00031070 with bits 16-23 zeroed = 0x00001070), keeping the same
     * white fill/pattern but drawing no border at all -- same technique
     * FM_DIV1/2/3's own 0x00001171L (framesize=0, interiorcol=1) already
     * relies on for a borderless solid bar, just with framecol/interiorcol
     * left as-is here since only the border needed to disappear, not the
     * fill. */
    fm_dlg[FM_ROOT].ob_spec.index = lm.short_screen ? 0x00001070L : 0x00031070L;

    /* The dialog title is the current directory (see fm_refresh()), not a
     * static "FLOPPY.PRG" -- the desktop's own menu bar already names the
     * running application across the top of the screen, so repeating it
     * here was pure duplication. */
    set_obj(fm_dlg, FM_TITLE, G_STRING, NONE, NORMAL, xl, yt, DW - 2*xl, lm.rh);
    fm_dlg[FM_TITLE].ob_spec.free_string = fm_title_text;

    set_obj(fm_dlg, FM_DIV1, G_BOX, NONE, NORMAL, xl, ydiv1, DW - 2*xl, 2);
    fm_dlg[FM_DIV1].ob_spec.index = 0x00001171L;
    if (lm.short_screen)
        fm_dlg[FM_DIV1].ob_flags |= (unsigned short)HIDETREE;

    set_obj(fm_dlg, FM_SOURCE_LINE, G_STRING, NONE, NORMAL, xl, ysource, DW - 2*xl, lm.rh);
    fm_dlg[FM_SOURCE_LINE].ob_spec.free_string = fm_source_line;

    set_obj(fm_dlg, FM_DIV2, G_BOX, NONE, NORMAL, xl, ydiv2, DW - 2*xl, 2);
    fm_dlg[FM_DIV2].ob_spec.index = 0x00001171L;

    for (i = 0; i < FM_MAX_VISIBLE_FILES; i++) {
        int ry = yrow0 + i * lm.pitch;
        set_obj(fm_dlg, FM_ROW(i), G_STRING, SELECTABLE | RBUTTON, NORMAL, xl, ry, DW - 2*xl, lm.rh);
        fm_dlg[FM_ROW(i)].ob_spec.free_string = fm_row_text[i];
    }

    set_obj(fm_dlg, FM_DIV3, G_BOX, NONE, NORMAL, xl, ydiv3, DW - 2*xl, 2);
    fm_dlg[FM_DIV3].ob_spec.index = 0x00001171L;
    if (lm.short_screen)
        fm_dlg[FM_DIV3].ob_flags |= (unsigned short)HIDETREE;

    /* [Source] used to be leftmost here -- moved to Favorites (see
     * FM_DIRUP_BTN's own enum comment). DirUp/Add/Back shifted left by
     * exactly the 14cw [Source] + gap used to occupy, so the gaps between
     * them are unchanged. */
    set_obj(fm_dlg, FM_DIRUP_BTN, G_BUTTON, EXIT | TOUCHEXIT, NORMAL, xl + 1*lm.cw, ybtn, 13*lm.cw, lm.rh);
    fm_dlg[FM_DIRUP_BTN].ob_spec.free_string = "   Dir Up   ";

    set_obj(fm_dlg, FM_ADD_BTN, G_BUTTON, EXIT | TOUCHEXIT, NORMAL, xl + 15*lm.cw, ybtn, 9*lm.cw, lm.rh);
    fm_dlg[FM_ADD_BTN].ob_spec.free_string = "   Add   ";

    set_obj(fm_dlg, FM_BACK_BTN, G_BUTTON, EXIT | TOUCHEXIT, NORMAL, xl + 25*lm.cw, ybtn, 8*lm.cw, lm.rh);
    fm_dlg[FM_BACK_BTN].ob_spec.free_string = " Back  ";

    /* Far right, reserved for pagination (Step 3+) -- no paging logic
     * behind these yet, see FM_PREV_BTN/FM_NEXT_BTN's own comment.
     * Anchored from the RIGHT edge of the real DW (not a fixed
     * char-multiple from the left) so they sit flush against the actual
     * right edge of the dialog regardless of what cw turns out to be at
     * runtime -- see this function's own top-of-function comment. */
    {
        int next_w = 8*lm.cw, prev_w = 8*lm.cw, btn_gap = 1*lm.cw;
        int x_next = DW - xl - next_w;
        int x_prev = x_next - btn_gap - prev_w;

        set_obj(fm_dlg, FM_PREV_BTN, G_BUTTON, EXIT | TOUCHEXIT, NORMAL, x_prev, ybtn, prev_w, lm.rh);
        fm_dlg[FM_PREV_BTN].ob_spec.free_string = "  Prev  ";

        set_obj(fm_dlg, FM_NEXT_BTN, G_BUTTON, EXIT | TOUCHEXIT, NORMAL, x_next, ybtn, next_w, lm.rh);
        fm_dlg[FM_NEXT_BTN].ob_spec.free_string = "  Next  ";
    }

    wire_tree(fm_dlg, FM_NOBJS);
}

/* Repopulates fm_row_text[] from fm_entries[]/fm_entry_count and
 * fm_title_text from fm_cwd -- the part of a refresh that never needs to
 * touch the browse session itself (BROWSE_OPEN/GET_*_PAGE), just render
 * whatever is already loaded. Used both by fm_refresh() (after a full
 * open+load) and directly after a navigation action (CHANGE_DIR) that
 * already updated fm_cwd/fm_entries itself.
 *
 * Directories are prefixed with the ST system font's own small folder
 * glyph (character 0x06, confirmed by inspecting the actual font in the
 * emulator -- the same icon the Atari desktop itself uses for a folder)
 * plus one space; files get two plain spaces in its place so every name,
 * directory or file, still lines up in the same text column. */
#define FM_DIR_ICON '\x06'
static void fm_apply_entries_to_rows(void)
{
    int i;

    sprintf(fm_title_text, "%.76s", fm_cwd);
    for (i = 0; i < FM_MAX_VISIBLE_FILES; i++) {
        if (i < fm_entry_count) {
            if (fm_entries[i].is_dir)
                sprintf(fm_row_text[i], "%c %.74s", FM_DIR_ICON, fm_entries[i].name);
            else
                sprintf(fm_row_text[i], "  %.74s", fm_entries[i].name);
            fm_dlg[FM_ROW(i)].ob_flags &= (unsigned short)(~HIDETREE);
        } else {
            fm_dlg[FM_ROW(i)].ob_flags |= (unsigned short)HIDETREE;
        }
        fm_dlg[FM_ROW(i)].ob_state &= (unsigned short)(~SELECTED);
    }
    fm_selected_row = -1;

    /* Same DISABLED convention already used elsewhere in this file (e.g.
     * FS_ROW, FP_DELETE) -- greyed out and unclickable rather than a live
     * button that would just alert "no such page" if clicked. */
    if (fm_has_prev)
        fm_dlg[FM_PREV_BTN].ob_state &= (unsigned short)(~DISABLED);
    else
        fm_dlg[FM_PREV_BTN].ob_state |= (unsigned short)DISABLED;
    if (fm_has_next)
        fm_dlg[FM_NEXT_BTN].ob_state &= (unsigned short)(~DISABLED);
    else
        fm_dlg[FM_NEXT_BTN].ob_state |= (unsigned short)DISABLED;
}

static void fm_refresh(ProfileConfig *cfg)
{
    const Profile *p;
    int have_active = (cfg->active_index >= 0 && cfg->active_index < MAX_PROFILES
                        && profile_slot_is_configured(&cfg->profiles[cfg->active_index]));

    fm_page_index = 0;

    if (have_active) {
        p = &cfg->profiles[cfg->active_index];
        sprintf(fm_source_line, "Source: %-.20s   Type: %s", p->nickname, profile_backend_word(p));

        if (fm_open_browse_for_active_profile(cfg)) {
            fm_load_entries();
        } else {
            fm_entry_count = 0;
            strncpy(fm_cwd, "/", sizeof(fm_cwd) - 1);
            fm_cwd[sizeof(fm_cwd) - 1] = '\0';
        }
    } else {
        /* No profile configured yet -- "/Floppies" is the placeholder
         * shown until a real source/directory exists, per the task
         * brief's own example. */
        fm_browse_ok = 0;
        fm_browse_profile_index = -1;
        fm_entry_count = 0;
        strncpy(fm_cwd, "/Floppies", sizeof(fm_cwd) - 1);
        fm_cwd[sizeof(fm_cwd) - 1] = '\0';
        sprintf(fm_source_line, "Source: (none selected)");
    }

    fm_apply_entries_to_rows();
}

/* Fills out_name with the browser's currently selected FILE name (never a
 * directory -- a folder is never a placeable favorite), or leaves it
 * empty if nothing valid is selected. Shared by every way of "confirming"
 * a selection inside fm_browse_run() below: double-click, Return/Enter
 * (fm_form_do_events()'s own is_double/keyboard handling), and the
 * [Add] button (FM_ADD_BTN's own case). A directory row
 * double-click never reaches this at all -- it navigates into the
 * directory instead. */
static void fm_capture_selection(char *out_name, int out_name_cap, char *out_dir, int out_dir_cap)
{
    if (fm_selected_row >= 0 && fm_selected_row < fm_entry_count
        && !fm_entries[fm_selected_row].is_dir) {
        set_buf(out_name, out_name_cap, fm_entries[fm_selected_row].name);
        set_buf(out_dir, out_dir_cap, fm_cwd);
    } else {
        out_name[0] = '\0';
        out_dir[0] = '\0';
    }
}

/* objc_find() only matches a click that lands exactly inside a row's own
 * rectangle -- the small gap between two rows (FM_MAX_VISIBLE_FILES rows
 * are spaced lm.pitch = lm.rh+3 apart but are only lm.rh tall, see
 * fm_dialog_init()) belongs to no object at all, so a click there is
 * silently a miss. This is the fallback used only when objc_find() itself
 * found nothing: it snaps to the nearest visible row if the click is
 * within 1px of that row's own top/bottom edge, so a stray click just off
 * a row still selects it. mx/my are absolute screen coordinates (as
 * evnt_multi() reports them); FM_ROW(i)'s own ob_x/ob_y are relative to
 * FM_ROOT (a flat, one-level tree, see wire_tree()), and FM_ROOT's own
 * ob_x/ob_y already hold its absolute screen position (set by
 * form_center()/dialog_open()) -- so the two are added together here to
 * get each row's absolute rectangle. Returns a row index, or -1 if the
 * click is nowhere near any visible row. */
static int fm_row_near(short mx, short my)
{
    int i;
    short root_x = fm_dlg[FM_ROOT].ob_x;
    short root_y = fm_dlg[FM_ROOT].ob_y;

    for (i = 0; i < fm_entry_count; i++) {
        OBJECT *row = &fm_dlg[FM_ROW(i)];
        short x = (short)(root_x + row->ob_x);
        short y = (short)(root_y + row->ob_y);

        if (mx < x || mx >= x + row->ob_width)
            continue;
        if (my >= y - 1 && my < y + row->ob_height + 1)
            return i;
    }
    return -1;
}

/* Erases just row's own rectangle (v_bar, white) before redrawing its
 * G_STRING -- same technique and same reason as fa_redraw_row() (see that
 * function's own comment): G_STRING objects paint text ink only, in
 * transparent mode, never a background, so a plain objc_draw() alone
 * cannot clear a stale SELECTED highlight (or old text) by itself. */
static void fm_redraw_row(int row)
{
    short cw, ch, bw, bh;
    short handle;
    short pxy[4];
    OBJECT *ro = &fm_dlg[FM_ROW(row)];
    short x = (short)(fm_dlg[FM_ROOT].ob_x + ro->ob_x);
    short y = (short)(fm_dlg[FM_ROOT].ob_y + ro->ob_y);

    handle = graf_handle(&cw, &ch, &bw, &bh);

    pxy[0] = x;
    pxy[1] = y;
    pxy[2] = (short)(x + ro->ob_width - 1);
    pxy[3] = (short)(y + ro->ob_height - 1);

    vswr_mode(handle, MD_REPLACE);
    vsf_interior(handle, FIS_SOLID);
    vsf_color(handle, WHITE);
    v_bar(handle, pxy);

    objc_draw(fm_dlg, FM_ROW(row), MAX_DEPTH, x, y, ro->ob_width, ro->ob_height);
}

/* Deselects the currently selected browser row, if any -- called on Esc
 * (fm_form_do_events()'s own MU_KEYBD handling below). Right-click
 * deselection was tried and dropped -- see fa_deselect()'s own comment.
 *
 * NOT form_button() re-clicked on the same row: FM_ROW is SELECTABLE |
 * RBUTTON, and EmuTOS's own fm_button() (the real implementation of
 * form_button(), aes/gemfmlib.c) shows the RBUTTON branch always
 * unconditionally re-ORs SELECTED onto the clicked object
 * (`tstate |= SELECTED`) and only clears OTHER siblings -- it never
 * XORs/toggles the clicked object itself off. That XOR-toggle only
 * happens in the plain-SELECTABLE (non-RBUTTON) branch. So calling
 * form_button() again on an already-selected RBUTTON member (as this
 * function first tried) just re-confirms it selected -- it can never
 * turn it off. This is why an earlier version of this function appeared
 * to do nothing at all when tested. The correct fix: clear the bit
 * directly and use fm_redraw_row()'s erase-then-draw, same technique
 * PLACE MODE placement already relies on for FA_ROW, since G_STRING has
 * no fill of its own to clear a highlight (or old text) any other way. */
static void fm_deselect(void)
{
    int row;

    if (fm_selected_row < 0)
        return;
    row = fm_selected_row;
    fm_dlg[FM_ROW(row)].ob_state &= (unsigned short)(~SELECTED);
    fm_selected_row = -1;
    fm_redraw_row(row);
}

/* Custom event loop instead of form_do(): needed for two things form_do()
 * alone cannot provide -- up/down arrow-key list navigation, and
 * double-click detection on a file row. Adapted from the same
 * evnt_multi()-based pattern SIDETNFS-Config's sw_form_do_ticking()
 * already established as this codebase's way to add behavior beyond
 * form_do()'s defaults. */
static short fm_form_do_events(int *out_double)
{
    short mx, my, mb, ks, kr, br;
    short msg[8];
    short event, obj, next;
    short result = -1;

    *out_double = 0;

    while (result == -1) {
        event = evnt_multi(MU_KEYBD | MU_BUTTON,
                           2, 1, 1,
                           0, 0, 0, 0, 0,
                           0, 0, 0, 0, 0,
                           msg,
                           0UL,
                           &mx, &my, &mb, &ks, &kr, &br);

        if (event & MU_BUTTON) {
            obj = objc_find(fm_dlg, FM_ROOT, MAX_DEPTH, mx, my);
            if (obj <= 0) {
                int near = fm_row_near(mx, my);
                if (near >= 0)
                    obj = (short)FM_ROW(near);
            }
            if (obj > 0) {
                next = obj;
                if (!form_button(fm_dlg, obj, br, &next)) {
                    result = (short)(next & 0x7FFF);
                    if (result > 0 && result < FM_NOBJS)
                        fm_dlg[result].ob_state &= (unsigned short)(~SELECTED);
                } else if (obj >= FM_ROW_BASE && obj < FM_AFTER_ROWS) {
                    *out_double = (br >= 2);
                    result = obj;
                }
            }
        }

        if (event & MU_KEYBD) {
            /* Standard AT/Atari keyboard scan codes, same convention the
             * old sidecart-configurator-atari project used for cursor
             * keys (KEY_UP_ARROW/KEY_DOWN_ARROW, helper.h) -- kr's high
             * byte is the scan code, low byte the ASCII value (0 for
             * non-ASCII keys like the arrows). */
            int scan = (kr >> 8) & 0x00FF;
            int visible_count = fm_entry_count;

            if (visible_count > 0 && (scan == 0x48 || scan == 0x50)) { /* up / down */
                int new_row = fm_selected_row;
                if (scan == 0x48) /* up */
                    new_row = (new_row <= 0) ? 0 : new_row - 1;
                else /* down */
                    new_row = (new_row < 0) ? 0 : ((new_row + 1 >= visible_count) ? visible_count - 1 : new_row + 1);

                /* Let form_button() do the highlight toggle -- same call
                 * the mouse-click path relies on (see fm_form_do_events()'s
                 * MU_BUTTON handling above). A first attempt here toggled
                 * the SELECTED bit by hand and redrew each row with a plain
                 * objc_draw(), which looked right for the newly-selected
                 * row but left the previously-selected row as a solid
                 * black bar: G_STRING has no fill of its own, so a "plain"
                 * redraw only re-draws the text glyphs (in black) on top of
                 * whatever is already there -- and what was already there
                 * was the SELECTED highlight's black fill, never erased.
                 * form_button() flips the highlight the way AES actually
                 * expects (an invert that undoes itself), which is exactly
                 * why the mouse path never had this problem. */
                if (new_row != fm_selected_row) {
                    short next = (short)FM_ROW(new_row);
                    form_button(fm_dlg, FM_ROW(new_row), 1, &next);
                    fm_selected_row = new_row;
                }
                /* result stays -1: handled here, keep looping */
            } else if ((kr & 0x00FF) == 0x0D) {
                /* Return/Enter ALWAYS does something now (Favorites
                 * task): with a row selected, same outcome as a
                 * double-click on it (open the directory, or confirm the
                 * file back to Favorites) -- reuses fm_browse_run()'s
                 * existing is_double handling there rather than
                 * duplicating navigate/confirm logic here. With nothing
                 * selected (or an empty directory), it still confirms
                 * (with nothing armed) -- FM_ADD_BTN's own dispatch
                 * in fm_browse_run() already handles that case correctly
                 * regardless of who "clicked" it, mouse or keyboard.
                 * ASCII 0x0D, not the scan code, since GEM already
                 * translates both the main Return key and the numpad
                 * Enter key to the same ASCII value. */
                if (visible_count > 0 && fm_selected_row >= 0 && fm_selected_row < visible_count) {
                    result = (short)FM_ROW(fm_selected_row);
                    *out_double = 1;
                } else {
                    result = (short)FM_ADD_BTN;
                }
            } else if (scan == 0x01 || (kr & 0x00FF) == 0x1B) {
                /* Esc: deselect -- see fm_deselect()'s own comment. Checked
                 * by BOTH scan code (0x01, standard AT/Atari Escape) and
                 * ASCII (0x1B): unlike Return/Enter, real hardware/AES was
                 * not confirmed to reliably report Escape's ASCII byte the
                 * way the arrow keys report none at all (scan-only) --
                 * this is defensive until confirmed which one actually
                 * fires here. */
                fm_deselect();
            } else if (scan == 0x0F || (kr & 0x00FF) == 0x09) {
                /* Tab: same as [Back] -- returns to Favorites with
                 * nothing armed, same as pressing FM_BACK_BTN itself
                 * (fm_browse_run()'s own case for it handles this
                 * uniformly regardless of mouse or keyboard origin, same
                 * idiom Enter already relies on for FM_ADD_BTN
                 * above). Scan code (standard AT/Atari Tab) and ASCII
                 * (0x09) both checked, same dual-check convention Esc
                 * already established. */
                result = (short)FM_BACK_BTN;
            } else if (scan == 0x4B) {
                /* Left arrow: same as [Prev] -- fm_browse_run()'s own
                 * case for it already checks fm_has_prev, so this is a
                 * harmless no-op when there is no previous page, same as
                 * the button itself being DISABLED then. */
                result = (short)FM_PREV_BTN;
            } else if (scan == 0x4D) {
                /* Right arrow: same as [Next] -- see the Left-arrow
                 * comment just above, mirrored for fm_has_next. */
                result = (short)FM_NEXT_BTN;
            } else if (scan == 0x0E || (kr & 0x00FF) == 0x08) {
                /* Backspace: same as [Dir Up]. Scan code (standard AT/
                 * Atari Backspace) and ASCII (0x08) both checked, same
                 * dual-check convention Esc/Tab already established. */
                result = (short)FM_DIRUP_BTN;
            } else if (scan == 0x61) {
                /* Undo: same as [Back] -- same scan code Favorites' own
                 * Undo-means-Quit uses, reused here for its own [Back]
                 * equivalent. */
                result = (short)FM_BACK_BTN;
            }
        }
    }

    return result;
}

/* Runs the file browser as a NESTED dialog (Favorites, dialog_run() below,
 * is the app's own top-level screen now -- see this file's own comment on
 * FA_QUIT_BTN/FA_BROWSER_BTN). Returns 1 if the user confirmed a file
 * (double-click, Return/Enter, or [Favorites] -- see fm_capture_selection())
 * -- out_name holds its name, out_name_cap bytes -- or 0 if they pressed
 * [Back] (or confirmed with nothing/a directory selected), in which case
 * out_name is left empty. Either way, control always returns to the
 * caller; this function never talks to the Favorites dialog directly. */
static int fm_browse_run(ProfileConfig *cfg, char *out_name, int out_name_cap,
                          char *out_dir, int out_dir_cap)
{
    DialogGeometry geo;
    short which;
    int done, confirmed;
    int is_double;

    fm_dialog_init();
    fm_refresh(cfg);
    dialog_open(fm_dlg, FM_ROOT, &geo, fm_short_screen);

    done = 0;
    confirmed = 0;
    while (!done) {
        which = fm_form_do_events(&is_double);

        if (which >= FM_ROW_BASE && which < FM_AFTER_ROWS) {
            /* form_button() (inside fm_form_do_events()) already toggled
             * SELECTED on this row -- and, being a member of an RBUTTON
             * family, deselected whichever row was selected before -- and
             * already redrew just those one or two row objects itself, the
             * normal GEM AES way. No objc_draw() here: redrawing the whole
             * FM_ROOT again on every single click was pure redundant work,
             * and on real (slow) hardware that redundant full-dialog
             * repaint was the visible flicker the user reported. */
            fm_selected_row = which - FM_ROW_BASE;
            if (is_double) {
                if (fm_selected_row < fm_entry_count && fm_entries[fm_selected_row].is_dir) {
                    /* Double-click on a directory row navigates into it --
                     * fm_change_dir() itself updates fm_gen/fm_cwd/
                     * last_directory on success and leaves them untouched
                     * on failure (already alerted). */
                    /* Busy cursor for the whole "go fetch the new
                     * directory over TNFS/SD and rebuild the row list"
                     * span -- an hourglass while it's working, back to the
                     * normal arrow only once the new screen is actually on
                     * screen, so a double-click always gives immediate
                     * feedback that it registered instead of the UI just
                     * sitting there for however long the round trip takes. */
                    graf_mouse(HOURGLASS, 0L);
                    if (fm_change_dir(cfg, 0, fm_entries[fm_selected_row].name)) {
                        fm_page_index = 0;
                        fm_load_entries();
                        fm_apply_entries_to_rows();
                    }
                    objc_draw(fm_dlg, FM_ROOT, MAX_DEPTH, geo.x, geo.y, geo.w, geo.h);
                    graf_mouse(ARROW, 0L);
                } else {
                    /* A file: confirm and return to the caller (Favorites,
                     * see dialog_run() below), same outcome as the
                     * [Add] button. */
                    fm_capture_selection(out_name, out_name_cap, out_dir, out_dir_cap);
                    confirmed = 1;
                    done = 1;
                }
            }
            continue;
        }

        switch (which) {
        case FM_DIRUP_BTN:
            /* Replaces the old separate Change Dir dialog's ".." row --
             * directories now show inline in the main row list (see
             * fm_apply_entries_to_rows()), so the only thing that dialog
             * still did that the row list itself can't is go up one level.
             * One button for that, no intermediate dialog. */
            if (cfg->active_index >= 0 && cfg->active_index < MAX_PROFILES
                && profile_slot_is_configured(&cfg->profiles[cfg->active_index]) && fm_browse_ok) {
                char root[PROFILE_LASTDIR_LEN];

                profile_root_dir(&cfg->profiles[cfg->active_index], root, sizeof(root));
                if (strcmp(fm_cwd, root) == 0) {
                    form_alert(1, "[3][Already at the top|directory.][OK]");
                } else {
                    graf_mouse(HOURGLASS, 0L);
                    if (fm_change_dir(cfg, 1, NULL)) {
                        fm_page_index = 0;
                        fm_load_entries();
                        fm_apply_entries_to_rows();
                    }
                    objc_draw(fm_dlg, FM_ROOT, MAX_DEPTH, geo.x, geo.y, geo.w, geo.h);
                    graf_mouse(ARROW, 0L);
                }
            } else {
                form_alert(1, "[3][No source selected.][OK]");
            }
            break;

        case FM_ADD_BTN:
            /* Confirm and return, selection or not -- see
             * fm_capture_selection()'s own comment. */
            fm_capture_selection(out_name, out_name_cap, out_dir, out_dir_cap);
            confirmed = 1;
            done = 1;
            break;

        case FM_PREV_BTN:
            /* fm_has_prev also gates the button's own DISABLED state (see
             * fm_apply_entries_to_rows()) -- checked again here too since
             * a DISABLED TOUCHEXIT button should never reach this dispatch
             * in the first place, but a no-op double-guard costs nothing. */
            if (fm_has_prev && fm_page_index > 0) {
                fm_page_index--;
                graf_mouse(HOURGLASS, 0L);
                fm_load_entries();
                fm_apply_entries_to_rows();
                objc_draw(fm_dlg, FM_ROOT, MAX_DEPTH, geo.x, geo.y, geo.w, geo.h);
                graf_mouse(ARROW, 0L);
            }
            break;

        case FM_NEXT_BTN:
            if (fm_has_next) {
                fm_page_index++;
                graf_mouse(HOURGLASS, 0L);
                fm_load_entries();
                fm_apply_entries_to_rows();
                objc_draw(fm_dlg, FM_ROOT, MAX_DEPTH, geo.x, geo.y, geo.w, geo.h);
                graf_mouse(ARROW, 0L);
            }
            break;

        case FM_BACK_BTN:
        default:
            /* Deliberate cancel: always returns with nothing armed, even
             * if a row happens to be selected -- distinct from
             * [Favorites]/double-click/Enter, which confirm the current
             * selection (or lack of one). */
            out_name[0] = '\0';
            out_dir[0] = '\0';
            confirmed = 0;
            done = 1;
            break;
        }
    }

    dialog_close(&geo);
    return confirmed;
}

/* Opens the nested browser (fm_browse_run()) and processes its result --
 * shared by [Browser] (FA_BROWSER_BTN, dialog_run()'s own MU_BUTTON
 * handling) and Tab (dialog_run()'s own MU_KEYBD handling). Returns the
 * new hand_shown value -- see fa_arm_move()'s own comment for why this
 * can't just update a caller's local directly. row_x/y/w/h are
 * recomputed here rather than passed in, same reasoning as
 * fa_arm_move(). */
static int fa_open_browser(ProfileConfig *cfg, int hand_shown)
{
    char browsed_name[FLOPPY_BROWSE_NAME_LEN];
    char browsed_dir[FLOPPY_BROWSE_CWD_LEN];
    short mx, my, mb, ks;
    short row_x, row_y, row_w, row_h;

    /* fm_browse_run() draws its own full-window dialog over (on a short
     * screen, exactly over) this screen's own area -- once it closes,
     * Favorites itself must be explicitly redrawn. No busy cursor:
     * fm_browse_run() is itself an interactive dialog, not a Pico round
     * trip. Also silently cancels a pending MOVE MODE, if one was armed
     * -- fa_move_source_slot is only ever read while fa_mode ==
     * FAVORITES_MODE_MOVE, so this reset is defensive tidiness, not a
     * correctness fix. */
    fa_move_source_slot = -1;
    if (fm_browse_run(cfg, browsed_name, (int)sizeof(browsed_name),
                       browsed_dir, (int)sizeof(browsed_dir))) {
        fa_mode = FAVORITES_MODE_PLACE;
        set_buf(fa_place_name, FLOPPY_BROWSE_NAME_LEN, browsed_name);
        set_buf(fa_place_dir, FLOPPY_BROWSE_CWD_LEN, browsed_dir);
    } else {
        fa_mode = FAVORITES_MODE_NORMAL;
        fa_place_name[0] = '\0';
        fa_place_dir[0] = '\0';
    }
    fa_update_title(cfg);
    fa_refresh_rows();
    objc_draw(fa_dlg, FA_ROOT, MAX_DEPTH, fa_dlg[FA_ROOT].ob_x, fa_dlg[FA_ROOT].ob_y,
              fa_dlg[FA_ROOT].ob_width, fa_dlg[FA_ROOT].ob_height);

    row_x = (short)(fa_dlg[FA_ROOT].ob_x + fa_dlg[FA_ROW(0)].ob_x);
    row_y = (short)(fa_dlg[FA_ROOT].ob_y + fa_dlg[FA_ROW(0)].ob_y);
    row_w = fa_dlg[FA_ROW(0)].ob_width;
    row_h = (short)((fa_dlg[FA_ROOT].ob_y + fa_dlg[FA_ROW(FA_ROWS - 1)].ob_y
                      + fa_dlg[FA_ROW(FA_ROWS - 1)].ob_height) - row_y);

    /* Cursor state must be recomputed from the mouse's ACTUAL position --
     * the browser fully owned the screen/cursor while open. */
    graf_mkstate(&mx, &my, &mb, &ks);
    hand_shown = (fa_mode == FAVORITES_MODE_PLACE
                  && mx >= row_x && mx < row_x + row_w
                  && my >= row_y && my < row_y + row_h);
    graf_mouse(hand_shown ? FLAT_HAND : ARROW, 0L);

    return hand_shown;
}

/* Moves fa_selected_row by one (up=0/down=1), clamped to 0..FA_ROWS-1,
 * toggling the highlight via form_button() the same way a mouse click on
 * a row would (see fm_form_do_events()'s own comment on why form_button()
 * rather than a hand-toggled SELECTED bit). Shared by NORMAL MODE's own
 * row-selection navigation and PLACE/MOVE MODE's own "which row will
 * Enter/Space act on" cursor, see fa_complete_place_or_move() below. */
static void fa_move_selection(int down)
{
    int new_row = fa_selected_row;

    if (!down)
        new_row = (new_row <= 0) ? 0 : new_row - 1;
    else
        new_row = (new_row < 0) ? 0 : ((new_row + 1 >= FA_ROWS) ? FA_ROWS - 1 : new_row + 1);

    if (new_row != fa_selected_row) {
        short next = (short)FA_ROW(new_row);
        form_button(fa_dlg, FA_ROW(new_row), 1, &next);
        fa_selected_row = new_row;
    }
}

/* Completes PLACE or MOVE MODE by acting on `row` (0..FA_ROWS-1, on the
 * currently displayed page) -- shared by the mouse-click path
 * (dialog_run()'s own MU_BUTTON handling, via fa_row_at()) and the
 * keyboard path (Up/Down + Enter/Space, see dialog_run()'s own MU_KEYBD
 * handling below), so a favorite can be placed or moved with the
 * keyboard alone, no mouse required. Caller is responsible for checking
 * fa_mode is actually PLACE or MOVE and row is valid first -- this
 * always ends whichever mode was active. */
static void fa_complete_place_or_move(int row)
{
    int slot = fa_current_page * FA_ROWS + row;

    /* Busy cursor for the whole place/move -- favcfg_write_entry()/
     * favcfg_move_entry() below do real file I/O (a streaming rewrite of
     * MOUNTn.CFG), which this session's own testing has shown can take a
     * noticeable moment on a TNFS-backed drive. Restored to ARROW once,
     * at the very end, regardless of which branch ran below. */
    graf_mouse(BUSY_BEE, 0L);

    if (fa_mode == FAVORITES_MODE_PLACE) {
        /* Persists immediately -- a streaming rewrite of this mount's own
         * MOUNTn.CFG, see favcfg_write_entry(). No separate Save step
         * exists anywhere in this app. */
        favcfg_write_entry(fa_mount_slot(), slot + 1, fa_place_name, fa_place_dir);
        fa_refresh_rows();
        fa_redraw_row(row); /* only this one row changed -- see fa_redraw_row()'s own comment on why a plain objc_draw() of just the row isn't enough by itself */

        /* Leave PLACE MODE, keep the dialog open with the new filename
         * visible -- exactly this task's own "AFTER PLACEMENT" contract.
         * Replacing an already-occupied slot needs no extra confirmation
         * step, also per this task's own brief. */
        fa_mode = FAVORITES_MODE_NORMAL;
        fa_place_name[0] = '\0';
        fa_place_dir[0] = '\0';
    } else if (fa_mode == FAVORITES_MODE_MOVE) {
        /* Same slot chosen again: a no-op move, but still ends MOVE
         * MODE -- confirming any row always completes the gesture one
         * way or another, same as PLACE MODE. */
        if (slot != fa_move_source_slot) {
            int source_page = fa_move_source_slot / FA_ROWS;
            int source_row = fa_move_source_slot % FA_ROWS;

            /* A true swap, not a one-way overwrite -- if the destination
             * already held a favorite, it moves to the source slot
             * rather than being lost (see favcfg_move_entry()'s own
             * comment). Persists immediately, same as Place/Erase. */
            favcfg_move_entry(fa_mount_slot(), fa_move_source_slot + 1, slot + 1);

            fa_refresh_rows(); /* current page only -- rebuilds fa_row_text[]/tab highlight from MOUNTn.CFG */
            fa_redraw_row(row); /* destination -- always on the current page */
            if (source_page == fa_current_page)
                fa_redraw_row(source_row); /* source also visible on this same page -- redraw it too */
        }

        fa_mode = FAVORITES_MODE_NORMAL;
        fa_move_source_slot = -1;
    }

    graf_mouse(ARROW, 0L);
}

/* Opens the server/source selector -- server_selector_run() (and
 * edit_servers_run() if that returns "edit") -- shared with the
 * browser's own former [Source] button, now moved here (FA_SOURCE_BTN,
 * dialog_run()'s own MU_BUTTON handling, and the S key). Refreshes the
 * title/Source button text and the favorites page afterward, since
 * switching the active source also switches which mount's favorites are
 * shown (fa_mount_slot() follows cfg->active_index directly). Silently
 * cancels any pending PLACE/MOVE MODE first, same defensive reasoning as
 * fa_open_browser()'s own comment -- a placement or move armed against
 * the OLD source no longer makes sense once the source itself changes. */
static void fa_open_source(ProfileConfig *cfg)
{
    int selector_result;

    fa_move_source_slot = -1;
    fa_mode = FAVORITES_MODE_NORMAL;
    fa_place_name[0] = '\0';
    fa_place_dir[0] = '\0';

    selector_result = server_selector_run(cfg);
    if (selector_result == 2)
        edit_servers_run(cfg);

    fa_update_title(cfg);
    fa_refresh_rows();
    objc_draw(fa_dlg, FA_ROOT, MAX_DEPTH, fa_dlg[FA_ROOT].ob_x, fa_dlg[FA_ROOT].ob_y,
              fa_dlg[FA_ROOT].ob_width, fa_dlg[FA_ROOT].ob_height);
}

/* The app's own top-level screen (see this file's own Favorites-dialog
 * section header comment) -- called once from main.c. Opens the browser
 * (fm_browse_run(), FM_* section above) as a nested dialog on [Browser];
 * a confirmed file arms PLACE MODE on return, otherwise NORMAL MODE
 * continues. [Quit] is the only way out, ending the whole program. */
void dialog_run(ProfileConfig *cfg)
{
    DialogGeometry geo;
    short mx, my, mb, ks, kr, br;
    short msg[8];
    short event, obj, next, which;
    short row_x, row_y, row_w, row_h;
    int done, hand_shown;

    /* "Please wait" goes up FIRST, before anything else -- favcfg_init()
     * (a couple of GEMDOS Dgetdrv()/Dgetpath() calls) and
     * favcfg_startup_validate() below (real FLOPPY.CFG file I/O, which
     * can be slow on a TNFS-backed drive) both used to run before this
     * was shown, or after it had already been hidden again, leaving a
     * silent, unexplained pause either side of the notice. Now the whole
     * startup I/O sequence -- firmware profile fetch AND favorites-config
     * load/validate -- runs while the SAME notice stays up throughout. */
    pw_dialog_show();

    fa_cfg = cfg;
    favcfg_init();

    shared_fields_init();

    startup_load(cfg);

    /* Restore the last source the user actually picked via [Source], if
     * it's still a configured slot -- picking a source only ever changes
     * cfg->active_index in RAM (see favcfg_write_active_slot()'s own
     * comment), so without this the firmware's own (flash-only, Save-
     * triggered) active profile is all startup_load() above has to go
     * on, which is not the same thing and can silently revert a source
     * switch the user never explicitly "Saved". A stale remembered slot
     * (no longer configured, or none ever remembered) leaves
     * cfg->active_index exactly as startup_load() set it. */
    {
        int remembered = favcfg_read_active_slot();
        if (remembered >= 1 && remembered <= MAX_PROFILES
            && profile_slot_is_configured(&cfg->profiles[remembered - 1]))
            cfg->active_index = remembered - 1;
    }

    /* cfg now holds the 8 mount slots as the firmware/FLASH itself just
     * reported them -- compare against CONFIG.CFG's own record of what
     * they were last time, and reset any slot that no longer matches
     * (mount reused for a different server/path/type/port since last
     * run) before a single favorite is shown. Missing/first-run files are
     * silently fine, never alerted -- see src/favcfg.c. */
    favcfg_startup_validate(cfg);

    /* fa_refresh_rows() (the very first page build) stays covered too --
     * it runs before dialog_open() below draws anything at all, so any
     * time it takes would otherwise show up as a blank screen rather
     * than the "please wait" notice that's already up anyway. */
    fa_dialog_init();
    fa_current_page = 0;
    fa_update_title(cfg);
    fa_refresh_rows();

    pw_dialog_hide();

    dialog_open(fa_dlg, FA_ROOT, &geo, fa_short_screen);

    fa_mode = FAVORITES_MODE_NORMAL; /* nothing browsed yet at program start */
    fa_place_name[0] = '\0';
    fa_place_dir[0] = '\0';

    /* Absolute screen rect of the whole row-list area (not per-row -- see
     * this task's own "FLAT_HAND while the mouse is over the favorite
     * file/slot area"), for MU_M1's enter/leave watch below. Computed once:
     * FA_ROOT's own ob_x/ob_y (and every row's relative ob_x/ob_y) never
     * change again after dialog_open() above, regardless of how many times
     * the nested browser is opened and closed below. */
    row_x = (short)(fa_dlg[FA_ROOT].ob_x + fa_dlg[FA_ROW(0)].ob_x);
    row_y = (short)(fa_dlg[FA_ROOT].ob_y + fa_dlg[FA_ROW(0)].ob_y);
    row_w = fa_dlg[FA_ROW(0)].ob_width;
    row_h = (short)((fa_dlg[FA_ROOT].ob_y + fa_dlg[FA_ROW(FA_ROWS - 1)].ob_y
                      + fa_dlg[FA_ROW(FA_ROWS - 1)].ob_height) - row_y);

    hand_shown = 0;
    graf_mouse(ARROW, 0L);

    done = 0;
    while (!done) {
        short flags = (short)(MU_KEYBD | MU_BUTTON);
        short m1leave = 0;

        /* FLAT_HAND hover is shared by PLACE and MOVE MODE -- see
         * FavoritesMode's own comment. */
        if (fa_mode == FAVORITES_MODE_PLACE || fa_mode == FAVORITES_MODE_MOVE) {
            flags = (short)(flags | MU_M1);
            m1leave = hand_shown ? 1 : 0;
        }

        event = evnt_multi(flags,
                           2, 1, 1,
                           m1leave, row_x, row_y, row_w, row_h,
                           0, 0, 0, 0, 0,
                           msg,
                           0UL,
                           &mx, &my, &mb, &ks, &kr, &br);

        if ((event & MU_M1) && (fa_mode == FAVORITES_MODE_PLACE || fa_mode == FAVORITES_MODE_MOVE)) {
            hand_shown = !hand_shown;
            graf_mouse(hand_shown ? FLAT_HAND : ARROW, 0L);
        }

        if (event & MU_BUTTON) {
            int placed = 0;

            if (fa_mode == FAVORITES_MODE_PLACE || fa_mode == FAVORITES_MODE_MOVE) {
                int row = fa_row_at(mx, my);
                if (row >= 0) {
                    fa_complete_place_or_move(row);
                    placed = 1;
                }
            }

            if (!placed) {
                obj = objc_find(fa_dlg, FA_ROOT, MAX_DEPTH, mx, my);
                if (obj > 0) {
                    next = obj;
                    if (!form_button(fa_dlg, obj, br, &next)) {
                        which = (short)(next & 0x7FFF);
                        if (which > 0 && which < FA_NOBJS)
                            fa_dlg[which].ob_state &= (unsigned short)(~SELECTED);
                        if (which >= FA_TAB_BASE && which < FA_AFTER_TABS) {
                            fa_switch_page(which - FA_TAB_BASE);
                        } else if (which == FA_SOURCE_BTN) {
                            fa_open_source(cfg);
                        } else if (which == FA_BROWSER_BTN) {
                            hand_shown = fa_open_browser(cfg, hand_shown);
                        } else if (which == FA_MOVE_BTN) {
                            /* See fa_arm_move()'s own comment -- no-op if
                             * nothing is selected. FLAT_HAND then shows
                             * over the row list (MU_M1 above), same as
                             * PLACE MODE, until a destination row is
                             * clicked. */
                            hand_shown = fa_arm_move(hand_shown);
                        } else if (which == FA_ERASE_BTN) {
                            fa_erase_selected();
                        } else if (which == FA_START_BTN) {
                            /* Same outcome as double-click/Enter on the
                             * selected favorite -- see fa_start_selected()'s
                             * own comment. */
                            fa_start_selected();
                        } else if (which == FA_QUIT_BTN) {
                            done = 1;
                        }
                    } else if (obj >= FA_ROW_BASE && obj < FA_AFTER_ROWS) {
                        /* SELECTABLE|RBUTTON, not EXIT -- form_button()
                         * already toggled the highlight (and cleared
                         * whichever row was selected before, being an
                         * RBUTTON family) and redrew just those objects
                         * itself, same as FM_ROW's own click handling
                         * (dialog_run()'s own comment there explains why
                         * no extra objc_draw() belongs here). A double-
                         * click (br>=2) shows the same stub alert
                         * Return/Enter does below -- see
                         * fa_start_selected()'s own comment. */
                        fa_selected_row = obj - FA_ROW_BASE;
                        if (br >= 2)
                            fa_start_selected();
                    }
                }
            }
        }

        if (event & MU_KEYBD) {
            int scan = (kr >> 8) & 0x00FF;

            /* Mode-independent shortcuts first -- these match what the
             * mouse can already do regardless of PLACE/MOVE MODE (Quit
             * and the tab buttons are reachable by mouse in any mode
             * today; Browser already cancels a pending PLACE/MOVE, see
             * fa_open_browser()'s own comment). Checked before the
             * NORMAL-MODE-only row-selection shortcuts below so
             * Ctrl+Del's own branch always wins over plain Delete's --
             * without that ordering, holding Ctrl while pressing Delete
             * would fire BOTH. */
            if (scan == 0x61) {
                /* Undo: same as [Quit] -- standard Atari ST scan code. */
                done = 1;
            } else if (scan == 0x0F || (kr & 0x00FF) == 0x09) {
                /* Tab: same as [Browser] -- scan code (standard AT/Atari
                 * Tab) and ASCII (0x09) both checked, same dual-check
                 * convention Esc already established. */
                hand_shown = fa_open_browser(cfg, hand_shown);
            } else if ((scan == 0x01 || (kr & 0x00FF) == 0x1B) && fa_mode != FAVORITES_MODE_NORMAL) {
                /* Esc while PLACE/MOVE MODE is armed (FLAT_HAND showing):
                 * cancel it and go back to plain ARROW/NORMAL MODE,
                 * without opening the browser the way Tab's own cancel
                 * does. Checked here, mode-independently, rather than
                 * folded into the NORMAL-MODE-only Esc/deselect handling
                 * further down -- that one only ever runs once fa_mode is
                 * already NORMAL, so it could never have caught this. */
                fa_mode = FAVORITES_MODE_NORMAL;
                fa_place_name[0] = '\0';
                fa_place_dir[0] = '\0';
                fa_move_source_slot = -1;
                graf_mouse(ARROW, 0L);
                hand_shown = 0;
                /* Also clear the keyboard-navigation row highlight, if
                 * Up/Down was used to pick a target before cancelling --
                 * see fa_move_selection()'s own comment on why PLACE/MOVE
                 * MODE reuses fa_selected_row for this. */
                if (fa_selected_row >= 0) {
                    fa_dlg[FA_ROW(fa_selected_row)].ob_state &= (unsigned short)(~SELECTED);
                    fa_redraw_row(fa_selected_row);
                    fa_selected_row = -1;
                }
            } else if (scan == 0x1F) {
                /* S: same as [Source] -- scan code, standard AT/Atari S,
                 * same shortcut the browser's own former [Source] button
                 * used before it moved here. */
                fa_open_source(cfg);
            } else if (scan == 0x3B) {
                fa_switch_page(0); /* F1 -> 01-15 */
            } else if (scan == 0x3C) {
                fa_switch_page(1); /* F2 -> 16-30 */
            } else if (scan == 0x3D) {
                fa_switch_page(2); /* F3 -> 31-45 */
            } else if (scan == 0x3E) {
                fa_switch_page(3); /* F4 -> 46-60 */
            } else if (scan == 0x53 && (ks & K_CTRL)) {
                /* Ctrl+Del: erase every slot on every page, with
                 * confirmation (unlike plain Erase/Delete, which never
                 * asks -- this one is irreversible across the WHOLE
                 * favorites list, not just one slot). Cancel is the
                 * default button (form_alert()'s own first argument) so
                 * an accidental Return/click doesn't wipe everything. */
                if (form_alert(2, "[3][Erase all 60 slots?][Erase|Cancel]") == 1) {
                    favcfg_erase_all(fa_mount_slot());
                    fa_refresh_rows();
                    objc_draw(fa_dlg, FA_ROOT, MAX_DEPTH, fa_dlg[FA_ROOT].ob_x, fa_dlg[FA_ROOT].ob_y,
                              fa_dlg[FA_ROOT].ob_width, fa_dlg[FA_ROOT].ob_height);
                }
            } else if (fa_mode == FAVORITES_MODE_NORMAL) {
                /* Row-selection-dependent shortcuts, NORMAL MODE only --
                 * PLACE/MOVE MODE has its own equivalent below, since
                 * there Up/Down/Enter act on the pending placement/move
                 * instead of a plain selection. */
                if (scan == 0x48 || scan == 0x50) { /* up / down */
                    fa_move_selection(scan == 0x50);
                } else if ((kr & 0x00FF) == 0x0D) {
                    /* Return/Enter on a selected favorite: same outcome as
                     * a double-click on it -- see fa_start_selected()'s
                     * own comment. ASCII 0x0D, not the scan code, same
                     * reasoning fm_form_do_events()'s own Enter handling
                     * already uses. */
                    fa_start_selected();
                } else if (scan == 0x53) {
                    /* Plain Delete key (Ctrl+Del already handled above):
                     * same outcome as clicking [Erase] -- see
                     * fa_erase_selected()'s own comment. */
                    fa_erase_selected();
                } else if (scan == 0x32) {
                    /* M key: same outcome as clicking [Move] -- see
                     * fa_arm_move()'s own comment. Scan code (standard
                     * AT/Atari code for M), not ASCII, same reasoning
                     * Esc's own dual check below settled on -- avoids
                     * depending on Shift state (M vs m) entirely. */
                    hand_shown = fa_arm_move(hand_shown);
                } else if (scan == 0x01 || (kr & 0x00FF) == 0x1B) {
                    /* Esc: deselect -- see fa_deselect()'s own comment.
                     * Checked by BOTH scan code (0x01) and ASCII (0x1B)
                     * -- see fm_form_do_events()'s own comment on this
                     * same check. */
                    fa_deselect();
                }
            } else {
                /* PLACE or MOVE MODE: Up/Down move the SAME fa_selected_row
                 * cursor NORMAL MODE uses (reusing fa_move_selection()), and
                 * Enter/Space confirm placing/moving onto whichever row is
                 * currently highlighted -- an alternative to clicking a row
                 * with the FLAT_HAND mouse cursor, not a replacement for it
                 * (the mouse path above still works exactly as before). A
                 * no-op with nothing highlighted yet (fa_selected_row still
                 * -1, e.g. the very first keypress after arming PLACE/MOVE
                 * MODE) -- press Up/Down at least once first. */
                if (scan == 0x48 || scan == 0x50) { /* up / down */
                    fa_move_selection(scan == 0x50);
                } else if ((kr & 0x00FF) == 0x0D || (kr & 0x00FF) == 0x20) {
                    if (fa_selected_row >= 0)
                        fa_complete_place_or_move(fa_selected_row);
                }
            }
        }
    }

    dialog_close(&geo);
}
