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
} LayoutMetrics;

static void layout_metrics_get(LayoutMetrics *lm)
{
    short bw, bh;
    short sx, sy, sw, sh;

    graf_handle(&lm->cw, &lm->ch, &bw, &bh);
    wind_get(0, WF_WORKXYWH, &sx, &sy, &sw, &sh);
    if (sh <= 0 || sh > 800) sh = 200;

    lm->rh    = (sh >= 350) ? (int)lm->ch : ((lm->ch > 8) ? lm->ch / 2 : (int)lm->ch);
    lm->tm    = (lm->rh / 4 > 2) ? lm->rh / 4 : 2;
    lm->pitch = lm->rh + 3;

    (void)sx; (void)sy; (void)sw; (void)bw; (void)bh;
}

/* The "run a modal dialog" open/close halves of the 5-step sequence every
 * editor in SIDETNFS-Config's dialog.c hand-copies (form_center ->
 * form_dial(FMD_START) -> objc_draw ... form_do loop ... ->
 * form_dial(FMD_FINISH)). The event loop itself (which button does what)
 * stays in each caller, since it is genuinely different per dialog -- only
 * the open/close boilerplate and the per-click TOUCHEXIT handling are
 * truly identical everywhere, so only those are factored here. */
typedef struct { short x, y, w, h; } DialogGeometry;

static void dialog_open(OBJECT *tree, int root_id, DialogGeometry *geo)
{
    form_center(tree, &geo->x, &geo->y, &geo->w, &geo->h);
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
    dialog_open(pw_dlg, PW_ROOT, &pw_geo);
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

    p->port = (int)w->port;
    strncpy(p->nickname, w->nickname, PROFILE_NICK_LEN - 1);
    p->nickname[PROFILE_NICK_LEN - 1] = '\0';
    strncpy(p->host, w->host, PROFILE_HOST_LEN - 1);
    p->host[PROFILE_HOST_LEN - 1] = '\0';
    strncpy(p->mount_path, w->mount_path, PROFILE_MOUNT_LEN - 1);
    p->mount_path[PROFILE_MOUNT_LEN - 1] = '\0';
    strncpy(p->last_directory, w->last_directory, PROFILE_LASTDIR_LEN - 1);
    p->last_directory[PROFILE_LASTDIR_LEN - 1] = '\0';
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

    w->port = (unsigned long)p->port;
    strncpy(w->nickname, p->nickname, FLOPPY_NICKNAME_LEN - 1);
    strncpy(w->host, p->host, FLOPPY_HOST_LEN - 1);
    strncpy(w->mount_path, p->mount_path, FLOPPY_MOUNTPATH_LEN - 1);
    strncpy(w->last_directory, p->last_directory, FLOPPY_LASTDIR_LEN - 1);
    /* NUL-termination of every field is already guaranteed by the
     * memset(w,0,...) above, same reasoning ui_to_wire_drive() gives. */
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
    if (!buf_nonempty(p->host)) {
        sprintf(msg, "[3][Validation error|Host is empty.][OK]");
        return 0;
    }
    if (p->port < 1 || p->port > 65535) {
        sprintf(msg, "[3][Validation error|Port must be 1-65535.][OK]");
        return 0;
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
/* ================================================================== */
enum {
    FP_ROOT = 0,
    FP_TITLE,
    FP_DIV1,
    FP_LBL_NICK,  FP_NICK_EDIT,
    FP_LBL_HOST,  FP_HOST_EDIT,
    FP_LBL_PORT,  FP_PORT_EDIT,
    FP_LBL_MOUNT, FP_MOUNT_EDIT, FP_MOUNT_HINT,
    FP_LBL_ACTIVE, FP_ACTIVE_BTN,
    FP_DIV2,
    FP_DELETE, FP_OK, FP_CANCEL,
    FP_NOBJS
};
static OBJECT fp_dlg[FP_NOBJS];

#define FP_BUF_NICK  PROFILE_NICK_LEN  /* 24 */
#define FP_BUF_HOST  PROFILE_HOST_LEN  /* 64 */
#define FP_BUF_PORT  7
#define FP_BUF_MOUNT PROFILE_MOUNT_LEN /* 32 */

static char buf_fp_nick [FP_BUF_NICK];
static char buf_fp_host [FP_BUF_HOST];
static char buf_fp_port [FP_BUF_PORT];
static char buf_fp_mount[FP_BUF_MOUNT];

static char tmpl_fp_nick[FP_BUF_NICK],   vld_fp_nick[FP_BUF_NICK];
static char tmpl_fp_host[FP_BUF_HOST],   vld_fp_host[FP_BUF_HOST];
static char tmpl_fp_port[FP_BUF_PORT],   vld_fp_port[FP_BUF_PORT];
static char tmpl_fp_mount[FP_BUF_MOUNT], vld_fp_mount[FP_BUF_MOUNT];

static TEDINFO ti_fp_nick, ti_fp_host, ti_fp_port, ti_fp_mount;

/* Active/Inactive toggle -- single button, own text doubles as the value
 * display, same "button with changing text" idiom SIDETNFS-Config's
 * update_drive_active_button_text() uses. */
#define FP_ACTIVE_BUF 11
static char buf_fp_active[FP_ACTIVE_BUF];
static int fp_editor_enabled; /* 1 = Active/ENABLED, 0 = Inactive/DISABLED -- live edit state */

static int fields_ready = 0;

static void shared_fields_init(void)
{
    if (fields_ready) return;
    fields_ready = 1;

    fill_n(tmpl_fp_nick,  '_', FP_BUF_NICK  - 1); fill_n(vld_fp_nick,  'X', FP_BUF_NICK  - 1);
    fill_n(tmpl_fp_host,  '_', FP_BUF_HOST  - 1); fill_n(vld_fp_host,  'X', FP_BUF_HOST  - 1);
    fill_n(tmpl_fp_port,  '_', FP_BUF_PORT  - 1); fill_n(vld_fp_port,  '9', FP_BUF_PORT  - 1);
    fill_n(tmpl_fp_mount, '_', FP_BUF_MOUNT - 1); fill_n(vld_fp_mount, 'X', FP_BUF_MOUNT - 1);

    init_ti(&ti_fp_nick,  buf_fp_nick,  tmpl_fp_nick,  vld_fp_nick,  FP_BUF_NICK);
    init_ti(&ti_fp_host,  buf_fp_host,  tmpl_fp_host,  vld_fp_host,  FP_BUF_HOST);
    init_ti(&ti_fp_port,  buf_fp_port,  tmpl_fp_port,  vld_fp_port,  FP_BUF_PORT);
    init_ti(&ti_fp_mount, buf_fp_mount, tmpl_fp_mount, vld_fp_mount, FP_BUF_MOUNT);
}

static void update_fp_active_button_text(void)
{
    strncpy(buf_fp_active, fp_editor_enabled ? " Active   " : " Inactive ", FP_ACTIVE_BUF - 1);
    buf_fp_active[FP_ACTIVE_BUF - 1] = '\0';
}

static void fp_dialog_init(int show_delete)
{
    LayoutMetrics lm;
    int DW, DH, xl, xf;
    int yt, ydiv1, ynick, yhost, yport, ymount, ymounthint, yactive, ydiv2, ybtn;

    layout_metrics_get(&lm);

    DW = 46 * lm.cw;
    xl = 2 * lm.cw;
    xf = 13 * lm.cw;

    yt         = lm.tm;
    ydiv1      = yt + lm.rh + 1;
    ynick      = ydiv1 + 5;
    yhost      = ynick + lm.pitch;
    yport      = yhost + lm.pitch;
    ymount     = yport + lm.pitch;
    ymounthint = ymount + lm.pitch;
    yactive    = ymounthint + lm.pitch;
    ydiv2      = yactive + lm.rh + 2;
    ybtn       = ydiv2 + 7;
    DH         = ybtn + lm.rh + lm.tm + 3;

    set_obj(fp_dlg, FP_ROOT, G_BOX, NONE, NORMAL, 0, 0, DW, DH);
    fp_dlg[FP_ROOT].ob_spec.index = 0x00031070L;

    set_obj(fp_dlg, FP_TITLE, G_STRING, NONE, NORMAL, 15*lm.cw, yt, 16*lm.cw, lm.rh);
    fp_dlg[FP_TITLE].ob_spec.free_string = "Floppy Server";

    set_obj(fp_dlg, FP_DIV1, G_BOX, NONE, NORMAL, lm.cw, ydiv1, DW - 2*lm.cw, 2);
    fp_dlg[FP_DIV1].ob_spec.index = 0x00001171L;

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
}

static void fp_load_from_profile(const Profile *p, int is_new)
{
    char port_str[FP_BUF_PORT];

    set_buf(buf_fp_nick,  FP_BUF_NICK, p->nickname);
    set_buf(buf_fp_host,  FP_BUF_HOST, p->host);
    sprintf(port_str, "%d", p->port);
    set_buf(buf_fp_port,  FP_BUF_PORT, port_str);
    set_buf(buf_fp_mount, FP_BUF_MOUNT, p->mount_path);

    /* A new (EMPTY) slot defaults to Active/ENABLED unless the user
     * explicitly toggles it off before OK -- same convention
     * SIDETNFS-Config's TNFS drive editor uses. */
    fp_editor_enabled = is_new ? 1 : (p->state == PROFILE_SLOT_ENABLED);
    update_fp_active_button_text();
}

static void fp_save_to_profile(Profile *p)
{
    buf_copy(buf_fp_nick,  p->nickname,   PROFILE_NICK_LEN);
    buf_copy(buf_fp_host,  p->host,       PROFILE_HOST_LEN);
    buf_copy(buf_fp_mount, p->mount_path, PROFILE_MOUNT_LEN);

    if (p->mount_path[0] == '\0') {
        p->mount_path[0] = '/';
        p->mount_path[1] = '\0';
    }

    p->port  = atoi(buf_fp_port); /* range-checked by validate_profile() */
    p->state = fp_editor_enabled ? PROFILE_SLOT_ENABLED : PROFILE_SLOT_DISABLED;
}

/* Returns 2 if the profile was removed (cleared to EMPTY), 1 if
 * added/modified, 0 if cancelled without changes. index is always a
 * valid fixed slot 0..MAX_PROFILES-1. */
static int fp_editor_run(ProfileConfig *cfg, int index)
{
    DialogGeometry geo;
    short which;
    int done;
    int is_new = profile_slot_is_empty(&cfg->profiles[index]);
    Profile working;
    char msg[200];

    if (is_new) {
        memset(&working, 0, sizeof(working));
        working.port = 16384; /* matches the reused firmware's own TNFS default */
        working.mount_path[0] = '/';
        working.mount_path[1] = '\0';
    } else {
        working = cfg->profiles[index];
    }

    fp_dialog_init(!is_new);
    fp_load_from_profile(&working, is_new);
    dialog_open(fp_dlg, FP_ROOT, &geo);

    done = 0;
    while (!done) {
        which = dialog_click(fp_dlg, FP_NICK_EDIT);

        switch (which) {
        case FP_ACTIVE_BTN:
            fp_editor_enabled = !fp_editor_enabled;
            update_fp_active_button_text();
            objc_draw(fp_dlg, FP_ROOT, MAX_DEPTH, geo.x, geo.y, geo.w, geo.h);
            break;

        case FP_DELETE:
            if (!is_new) {
                if (form_alert(1, "[2][Remove this server profile?][Remove|Cancel]") == 1)
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

    DW = 46 * lm.cw;
    yt    = lm.tm;
    ydiv1 = yt + lm.rh + 1;
    yrow0 = ydiv1 + 5;
    ydiv2 = yrow0 + MAX_PROFILES * lm.pitch + 2;
    ybtn  = ydiv2 + 7;
    DH    = ybtn + lm.rh + lm.tm + 3;

    set_obj(fe_dlg, FE_ROOT, G_BOX, NONE, NORMAL, 0, 0, DW, DH);
    fe_dlg[FE_ROOT].ob_spec.index = 0x00031070L;

    set_obj(fe_dlg, FE_TITLE, G_STRING, NONE, NORMAL, 12*lm.cw, yt, 22*lm.cw, lm.rh);
    fe_dlg[FE_TITLE].ob_spec.free_string = "Edit Floppy Servers";

    set_obj(fe_dlg, FE_DIV1, G_BOX, NONE, NORMAL, lm.cw, ydiv1, DW - 2*lm.cw, 2);
    fe_dlg[FE_DIV1].ob_spec.index = 0x00001171L;

    for (i = 0; i < MAX_PROFILES; i++) {
        int ry = yrow0 + i * lm.pitch;

        set_obj(fe_dlg, FE_ROW_TEXT(i), G_STRING, NONE, NORMAL, lm.cw, ry, 32*lm.cw, lm.rh);
        fe_dlg[FE_ROW_TEXT(i)].ob_spec.free_string = fe_row_text[i];

        set_obj(fe_dlg, FE_ROW_BTN(i), G_BUTTON, EXIT | TOUCHEXIT, NORMAL, 34*lm.cw, ry, 8*lm.cw, lm.rh);
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
            const char *active_mark = (i == cfg->active_index) ? "*" : " ";
            sprintf(fe_row_text[i], "%d%s %-8s %-22.22s", i + 1, active_mark, state_word, p->nickname);
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
    dialog_open(fe_dlg, FE_ROOT, &geo);

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
    fs_dlg[FS_EDIT].ob_spec.free_string = "Edit servers...";

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
            sprintf(fs_row_text[i], "%s%-.20s", (i == cfg->active_index) ? "> " : "  ", p->nickname);
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
    dialog_open(fs_dlg, FS_ROOT, &geo);

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

/* ================================================================== */
/* Main window (FM_*)                                                  */
/* ================================================================== */
enum {
    FM_ROOT = 0,
    FM_TITLE,
    FM_DIV1,
    FM_ACTIVE_LBL, FM_ACTIVE_VAL,
    FM_DIV2,
    FM_SERVER, FM_QUIT,
    FM_NOBJS
};
static OBJECT fm_dlg[FM_NOBJS];

#define FM_ACTIVE_BUF 28
static char fm_active_val[FM_ACTIVE_BUF];

static void fm_dialog_init(void)
{
    LayoutMetrics lm;
    int DW, DH;
    int yt, ydiv1, yactive, ydiv2, ybtn;

    layout_metrics_get(&lm);

    DW = 36 * lm.cw;
    yt      = lm.tm;
    ydiv1   = yt + lm.rh + 1;
    yactive = ydiv1 + 5;
    ydiv2   = yactive + lm.rh + 2;
    ybtn    = ydiv2 + 7;
    DH      = ybtn + lm.rh + lm.tm + 3;

    set_obj(fm_dlg, FM_ROOT, G_BOX, NONE, NORMAL, 0, 0, DW, DH);
    fm_dlg[FM_ROOT].ob_spec.index = 0x00031070L;

    set_obj(fm_dlg, FM_TITLE, G_STRING, NONE, NORMAL, 12*lm.cw, yt, 12*lm.cw, lm.rh);
    fm_dlg[FM_TITLE].ob_spec.free_string = "FLOPPY.PRG";

    set_obj(fm_dlg, FM_DIV1, G_BOX, NONE, NORMAL, lm.cw, ydiv1, DW - 2*lm.cw, 2);
    fm_dlg[FM_DIV1].ob_spec.index = 0x00001171L;

    set_obj(fm_dlg, FM_ACTIVE_LBL, G_STRING, NONE, NORMAL, lm.cw, yactive, 15*lm.cw, lm.rh);
    fm_dlg[FM_ACTIVE_LBL].ob_spec.free_string = "Active server:";
    set_obj(fm_dlg, FM_ACTIVE_VAL, G_STRING, NONE, NORMAL, 16*lm.cw, yactive, 18*lm.cw, lm.rh);
    fm_dlg[FM_ACTIVE_VAL].ob_spec.free_string = fm_active_val;

    set_obj(fm_dlg, FM_DIV2, G_BOX, NONE, NORMAL, lm.cw, ydiv2, DW - 2*lm.cw, 2);
    fm_dlg[FM_DIV2].ob_spec.index = 0x00001171L;

    set_obj(fm_dlg, FM_SERVER, G_BUTTON, EXIT | TOUCHEXIT, NORMAL, 4*lm.cw, ybtn, 14*lm.cw, lm.rh);
    fm_dlg[FM_SERVER].ob_spec.free_string = "[S]erver";

    set_obj(fm_dlg, FM_QUIT, G_BUTTON, EXIT | TOUCHEXIT, NORMAL, 20*lm.cw, ybtn, 12*lm.cw, lm.rh);
    fm_dlg[FM_QUIT].ob_spec.free_string = "  Quit  ";

    wire_tree(fm_dlg, FM_NOBJS);
}

static void fm_refresh(const ProfileConfig *cfg)
{
    const Profile *p;

    if (cfg->active_index >= 0 && cfg->active_index < MAX_PROFILES
        && profile_slot_is_configured(&cfg->profiles[cfg->active_index])) {
        p = &cfg->profiles[cfg->active_index];
        sprintf(fm_active_val, "%-.20s", p->nickname);
    } else {
        sprintf(fm_active_val, "(none selected)");
    }
}

/* Custom event loop instead of form_do(), adapted directly from
 * SIDETNFS-Config's sw_form_do_ticking() (the one place that codebase
 * already needed something other than form_do() to add an extra keyboard
 * shortcut) -- here to add the 'S' shortcut for [Server], per the task
 * brief, without inventing an unproven keyboard-handling mechanism. No
 * timer tick is needed here (unlike sw_form_do_ticking()'s clock), so
 * MU_TIMER is left out of the event mask entirely. */
static short fm_form_do_events(void)
{
    short mx, my, mb, ks, kr, br;
    short msg[8];
    short event, obj, next;
    short result = -1;

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
                }
            }
        }

        if (event & MU_KEYBD) {
            int c = kr & 0x00FF;
            if (c == 's' || c == 'S')
                result = FM_SERVER;
        }
    }

    return result;
}

void dialog_run(ProfileConfig *cfg)
{
    DialogGeometry geo;
    short which;
    int done;
    int selector_result;

    shared_fields_init();

    pw_dialog_show();
    startup_load(cfg);
    pw_dialog_hide();

    fm_dialog_init();
    fm_refresh(cfg);
    dialog_open(fm_dlg, FM_ROOT, &geo);

    done = 0;
    while (!done) {
        which = fm_form_do_events();

        switch (which) {
        case FM_SERVER:
            selector_result = server_selector_run(cfg);
            if (selector_result == 2)
                edit_servers_run(cfg);
            fm_refresh(cfg);
            objc_draw(fm_dlg, FM_ROOT, MAX_DEPTH, geo.x, geo.y, geo.w, geo.h);
            break;

        case FM_QUIT:
        default:
            done = 1;
            break;
        }
    }

    dialog_close(&geo);
}
