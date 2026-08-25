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

/* Wait for left mouse button to be released (for use after TOUCHEXIT). */
static void wait_mouse_release(void)
{
    short mx, my, mb, mk;
    do { graf_mkstate(&mx, &my, &mb, &mk); } while (mb & 1);
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

/* One form_do() call plus the mandatory TOUCHEXIT mouse-release wait
 * (see the file header comment). Returns the clicked object's id. */
static short dialog_click(OBJECT *tree, int start_obj)
{
    short which = (short)(form_do(tree, start_obj) & 0x7FFF);
    wait_mouse_release();
    return which;
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
static int fp_editor_run(ProfileConfig *cfg, int index)
{
    DialogGeometry geo;
    short which;
    int done;
    int first_form_do = 1;
    int is_new = profile_slot_is_empty(&cfg->profiles[index]);
    Profile working;
    char msg[200];

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

    done = 0;
    while (!done) {
        /* FP_NICK_EDIT only as the edit object on the FIRST form_do() of
         * this dialog session (auto-focuses Nickname the instant it
         * opens) -- every SUBSEQUENT re-entry (after SOURCE_TNFS_BTN/
         * SOURCE_SD_BTN/FP_ACTIVE_BTN, all EXIT objects that return here
         * and loop back into another dialog_click()) uses FP_ROOT
         * instead, same as every other dialog in this file (FE_ROOT/
         * FS_ROOT). Repeatedly re-passing the SAME edit object across
         * many form_do() calls on this dialog reproducibly crashed on
         * real hardware (three bombs) as soon as the user then clicked
         * into that field -- re-entering edit mode on an object that was
         * already left in edit mode by a previous form_do() call is not
         * a pattern any other dialog here relies on. Clicking the
         * Nickname box by hand still enters edit mode normally either
         * way -- this only gives up the "already focused" convenience on
         * the very first frame after a button click. */
        which = dialog_click(fp_dlg, first_form_do ? FP_NICK_EDIT : FP_ROOT);
        first_form_do = 0;

        switch (which) {
        case FP_SOURCE_TNFS_BTN:
            fp_editor_backend = PROFILE_BACKEND_TNFS;
            update_fp_source_buttons();
            fp_apply_backend_visibility();
            objc_draw(fp_dlg, FP_ROOT, MAX_DEPTH, geo.x, geo.y, geo.w, geo.h);
            break;

        case FP_SOURCE_SD_BTN:
            fp_editor_backend = PROFILE_BACKEND_SD;
            update_fp_source_buttons();
            fp_apply_backend_visibility();
            objc_draw(fp_dlg, FP_ROOT, MAX_DEPTH, geo.x, geo.y, geo.w, geo.h);
            break;

        case FP_ACTIVE_BTN:
            fp_editor_enabled = !fp_editor_enabled;
            update_fp_active_button_text();
            objc_draw(fp_dlg, FP_ROOT, MAX_DEPTH, geo.x, geo.y, geo.w, geo.h);
            break;

        case FP_DELETE:
            if (!is_new) {
                if (form_alert(1, "[2][Remove this floppy source?][Remove|Cancel]") == 1)
                    done = 2;
            }
            break;

        case FP_OK:
            fp_save_to_profile(&working);
            if (!validate_profile(&working, msg)) {
                form_alert(1, msg);
                break;
            }
            cfg->profiles[index] = working;
            done = 1;
            break;

        case FP_CANCEL:
        default:
            done = 3;
            break;
        }
    }

    dialog_close(&geo);

    if (done == 2) {
        memset(&cfg->profiles[index], 0, sizeof(cfg->profiles[index])); /* state == PROFILE_SLOT_EMPTY */
        if (cfg->active_index == index)
            cfg->active_index = 0;
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

static void edit_servers_run(ProfileConfig *cfg)
{
    DialogGeometry geo;
    short which;
    int done;
    char msg[220];

    fe_dialog_init();
    fe_refresh_rows(cfg);
    dialog_open(fe_dlg, FE_ROOT, &geo, 0);

    done = 0;
    while (!done) {
        which = dialog_click(fe_dlg, FE_ROOT);

        if (which == FE_SAVE) {
            perform_save(cfg, msg);
            form_alert(1, msg);
        } else if (which >= FE_ROW_BASE && which < FE_AFTER_ROWS) {
            int obj_offset = which - FE_ROW_BASE;
            int slot = obj_offset / 2;
            int is_btn = (obj_offset % 2) == 1;
            if (is_btn) {
                fp_editor_run(cfg, slot);
                fe_refresh_rows(cfg);
                objc_draw(fe_dlg, FE_ROOT, MAX_DEPTH, geo.x, geo.y, geo.w, geo.h);
            }
        } else if (which == FE_OK || which == FE_CANCEL) {
            done = 1;
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

/* Returns 1 if the user picked a profile (cfg->active_index updated,
 * locally only -- see perform_save()'s own comment on when this actually
 * reaches flash), 2 if the user asked to edit the server list instead
 * (caller then calls edit_servers_run()), 0 if cancelled. */
static int server_selector_run(ProfileConfig *cfg)
{
    DialogGeometry geo;
    short which;
    int result = 0;

    fs_dialog_init();
    fs_refresh_rows(cfg);
    dialog_open(fs_dlg, FS_ROOT, &geo, 0);

    for (;;) {
        which = dialog_click(fs_dlg, FS_ROOT);

        if (which >= FS_ROW_BASE && which < FS_AFTER_ROWS) {
            int slot = which - FS_ROW_BASE;
            if (profile_slot_is_configured(&cfg->profiles[slot])) {
                cfg->active_index = slot;
                result = 1;
                break;
            }
        } else if (which == FS_EDIT) {
            result = 2;
            break;
        } else if (which == FS_CANCEL) {
            result = 0;
            break;
        }
    }

    dialog_close(&geo);
    return result;
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

/* Fills fm_entries[]/fm_entry_count from page 0 of the active CWD's
 * directory listing (first, up to FM_MAX_VISIBLE_FILES) then, if room is
 * left, page 0 of the file listing -- no pagination yet (Step 3), so a
 * directory with more entries than fit just shows its first
 * FM_MAX_VISIBLE_FILES dirs-then-files and nothing beyond that. A
 * FLOPPY_PROBE_OK communication result with a non-OK/non-END_OF_DIRECTORY
 * browse status is alerted once; count is always trustworthy as 0 in
 * every error case (sidetnfs_floppy_browse_get_page()'s own contract), so
 * the list is simply left empty rather than guessed at. */
static void fm_load_entries(void)
{
    FloppyPageResult dp, fp;
    int i, n;

    fm_entry_count = 0;
    if (!fm_browse_ok)
        return;

    if (floppy_probe_browse_get_dir_page(fm_gen, 0UL, &dp) != FLOPPY_PROBE_OK) {
        form_alert(1, "[3][Could not reach the|cartridge (timeout).][OK]");
        return;
    }
    if (dp.status != FLOPPY_BROWSE_OK && dp.status != FLOPPY_BROWSE_STATUS_END_OF_DIRECTORY) {
        char msg[96];
        sprintf(msg, "[3][Could not list directories|(error %lu).][OK]", dp.status);
        form_alert(1, msg);
        return;
    }
    n = (int)dp.count;
    if (n > FM_MAX_VISIBLE_FILES) n = FM_MAX_VISIBLE_FILES;
    for (i = 0; i < n; i++) {
        strncpy(fm_entries[fm_entry_count].name, dp.entries[i], FLOPPY_BROWSE_NAME_LEN - 1);
        fm_entries[fm_entry_count].name[FLOPPY_BROWSE_NAME_LEN - 1] = '\0';
        fm_entries[fm_entry_count].is_dir = 1;
        fm_entry_count++;
    }

    if (fm_entry_count >= FM_MAX_VISIBLE_FILES)
        return;

    if (floppy_probe_browse_get_file_page(fm_gen, 0UL, &fp) != FLOPPY_PROBE_OK) {
        form_alert(1, "[3][Could not reach the|cartridge (timeout).][OK]");
        return;
    }
    if (fp.status != FLOPPY_BROWSE_OK && fp.status != FLOPPY_BROWSE_STATUS_END_OF_DIRECTORY) {
        char msg[96];
        sprintf(msg, "[3][Could not list files|(error %lu).][OK]", fp.status);
        form_alert(1, msg);
        return;
    }
    n = (int)fp.count;
    for (i = 0; i < n && fm_entry_count < FM_MAX_VISIBLE_FILES; i++) {
        strncpy(fm_entries[fm_entry_count].name, fp.entries[i], FLOPPY_BROWSE_NAME_LEN - 1);
        fm_entries[fm_entry_count].name[FLOPPY_BROWSE_NAME_LEN - 1] = '\0';
        fm_entries[fm_entry_count].is_dir = 0;
        fm_entry_count++;
    }
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
#define FM_SOURCE_BTN    (FM_AFTER_ROWS + 1)
#define FM_DIRUP_BTN     (FM_AFTER_ROWS + 2)
#define FM_START_BTN     (FM_AFTER_ROWS + 3)
#define FM_QUIT_BTN      (FM_AFTER_ROWS + 4)
/* Placeholder for future pagination (Step 3+) -- buttons only for now, no
 * paging logic wired in yet: GET_DIR_PAGE/GET_FILE_PAGE already support
 * an arbitrary page_index on the protocol/firmware side, but fm_load_entries()
 * only ever asks for page 0. Clicking either does nothing yet. */
#define FM_PREV_BTN      (FM_AFTER_ROWS + 5)
#define FM_NEXT_BTN      (FM_AFTER_ROWS + 6)
#define FM_NOBJS         (FM_AFTER_ROWS + 7)
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
#define FM_SOURCE_BTN_BUF 16
static char fm_source_btn_text[FM_SOURCE_BTN_BUF]; /* "Source: TNFS" / "Source: SD" -- own text doubles as the value, same idiom as Active/Source toggles elsewhere */
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

    set_obj(fm_dlg, FM_SOURCE_BTN, G_BUTTON, EXIT | TOUCHEXIT, NORMAL, xl + 1*lm.cw, ybtn, 13*lm.cw, lm.rh);
    fm_dlg[FM_SOURCE_BTN].ob_spec.free_string = fm_source_btn_text;

    set_obj(fm_dlg, FM_DIRUP_BTN, G_BUTTON, EXIT | TOUCHEXIT, NORMAL, xl + 15*lm.cw, ybtn, 13*lm.cw, lm.rh);
    fm_dlg[FM_DIRUP_BTN].ob_spec.free_string = "   Dir Up   ";

    set_obj(fm_dlg, FM_START_BTN, G_BUTTON, EXIT | TOUCHEXIT, NORMAL, xl + 29*lm.cw, ybtn, 9*lm.cw, lm.rh);
    fm_dlg[FM_START_BTN].ob_spec.free_string = "  Start  ";

    set_obj(fm_dlg, FM_QUIT_BTN, G_BUTTON, EXIT | TOUCHEXIT, NORMAL, xl + 39*lm.cw, ybtn, 8*lm.cw, lm.rh);
    fm_dlg[FM_QUIT_BTN].ob_spec.free_string = " Quit  ";

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
}

static void fm_refresh(ProfileConfig *cfg)
{
    const Profile *p;
    int have_active = (cfg->active_index >= 0 && cfg->active_index < MAX_PROFILES
                        && profile_slot_is_configured(&cfg->profiles[cfg->active_index]));

    if (have_active) {
        p = &cfg->profiles[cfg->active_index];
        sprintf(fm_source_line, "Source: %-.20s   Type: %s", p->nickname, profile_backend_word(p));
        sprintf(fm_source_btn_text, "Source: %s", profile_backend_word(p));

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
        strncpy(fm_source_btn_text, "Source", sizeof(fm_source_btn_text) - 1);
        fm_source_btn_text[sizeof(fm_source_btn_text) - 1] = '\0';
    }

    fm_apply_entries_to_rows();
}

/* STEP 1.x STUB -- there is no floppy mount/emulation yet (explicitly out
 * of scope, see RESEARCH-STEP0.md). Builds the full source+directory+
 * filename path (no GEMDOS 8.3 shortening -- these are plain C strings,
 * never touched by GEMDOS) and reports what would be started. Deliberately
 * kept to exactly the inputs a real "send the selection to the Pico"
 * command will need (source/backend + directory + filename), so wiring up
 * the real command later should not require changing this function's
 * shape, only its body. */
static void fm_start_selected(const ProfileConfig *cfg)
{
    const Profile *p;
    char msg[400];

    if (fm_selected_row < 0 || fm_selected_row >= fm_entry_count)
        return; /* Start with no selection: do nothing, matches the task brief */
    if (fm_entries[fm_selected_row].is_dir)
        return; /* a directory row is navigated (see dialog_run()'s double-click handling), never "started" */

    p = &cfg->profiles[cfg->active_index];

    sprintf(msg, "[1][Would start:|%-.20s (%s)|%-.60s/|%-.30s][OK]",
            p->nickname, profile_backend_word(p), fm_cwd, fm_entries[fm_selected_row].name);
    form_alert(1, msg);
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
            }
        }
    }

    return result;
}

void dialog_run(ProfileConfig *cfg)
{
    DialogGeometry geo;
    short which;
    int done;
    int is_double;
    int selector_result;

    shared_fields_init();

    pw_dialog_show();
    startup_load(cfg);
    pw_dialog_hide();

    fm_dialog_init();
    fm_refresh(cfg);
    dialog_open(fm_dlg, FM_ROOT, &geo, fm_short_screen);

    done = 0;
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
                        fm_load_entries();
                        fm_apply_entries_to_rows();
                    }
                    objc_draw(fm_dlg, FM_ROOT, MAX_DEPTH, geo.x, geo.y, geo.w, geo.h);
                    graf_mouse(ARROW, 0L);
                } else {
                    fm_start_selected(cfg);
                }
            }
            continue;
        }

        switch (which) {
        case FM_SOURCE_BTN:
            /* server_selector_run()/edit_servers_run() are themselves
             * interactive dialogs -- the user is choosing, not waiting on
             * the Pico, so they keep the normal arrow. The busy cursor
             * only brackets fm_refresh()'s own TNFS/SD round trip and the
             * resulting redraw. */
            selector_result = server_selector_run(cfg);
            if (selector_result == 2)
                edit_servers_run(cfg);
            graf_mouse(HOURGLASS, 0L);
            fm_refresh(cfg);
            objc_draw(fm_dlg, FM_ROOT, MAX_DEPTH, geo.x, geo.y, geo.w, geo.h);
            graf_mouse(ARROW, 0L);
            break;

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

        case FM_START_BTN:
            if (fm_selected_row < 0)
                form_alert(1, "[3][No image selected.][OK]");
            else
                fm_start_selected(cfg);
            break;

        case FM_PREV_BTN:
        case FM_NEXT_BTN:
            /* Reserved for pagination (Step 3+) -- intentionally a no-op
             * for now, see FM_PREV_BTN/FM_NEXT_BTN's own comment. */
            break;

        case FM_QUIT_BTN:
        default:
            done = 1;
            break;
        }
    }

    dialog_close(&geo);
}
