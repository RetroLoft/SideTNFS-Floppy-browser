/*
 * FLOPPY.PRG local persistence: Browser Sources (CONFIG.CFG), the last
 * active source (ACTIVE.CFG), and the global Favorites catalog
 * (FAVORITS.CFG).
 *
 * Deliberately 1980s-plain: plain text, one value per line, a STRICT
 * fixed line order per record, no keywords, no headers, no magic values,
 * no version fields, no CRC, no binary records.
 *
 * Architecture note: CONFIG.CFG used to be only a secondary "last-seen
 * mount snapshot" -- Browser Sources themselves lived in the Pico's own
 * flash (GET_CONFIG_INFO/GET_PROFILE/SET_PROFILE/SAVE_PROFILES). That
 * whole flash-backed profile store is gone from the firmware now;
 * CONFIG.CFG is the SOLE source of truth for Browser Sources, read/
 * written directly here, with zero cartridge round-trips for source
 * management. Favorites changed shape in the same architecture change:
 * each Favorite is now fully self-contained (its own backend+host+port+
 * complete path) instead of living inside one particular source's own
 * file (the old MOUNTn.CFG, one per mount slot) -- so a Favorite never
 * goes stale when a Source is edited or removed, and there is no more
 * per-source favorites file or startup mismatch-detection to speak of.
 *
 * Layout on disk, next to FLOPPY.PRG itself (see favcfg_init()):
 *
 *   FLOPPY.CFG\CONFIG.CFG   -- the 8 Browser Source slots. Fixed 8-line
 *                              record per slot, 8 slots, no keywords:
 *                                state       ("EMPTY"/"DISABLED"/"ENABLED")
 *                                backend     ("TNFS"/"SDCARD", "" if EMPTY)
 *                                nickname
 *                                host        (TNFS only, else "")
 *                                port        (TNFS only, else "0")
 *                                browser start directory (TNFS only, else "")
 *                                SD path     (SD only, else "")
 *                                last browsed directory
 *                              An EMPTY slot is "EMPTY" then 7 blank
 *                              lines. A missing file means "8 EMPTY
 *                              slots", not an error -- a genuine first
 *                              run (or an old pre-architecture-change
 *                              install).
 *
 *   FLOPPY.CFG\ACTIVE.CFG   -- one bare line, the last actively selected
 *                              source slot (1..8). See its own section
 *                              below.
 *
 *   FLOPPY.CFG\FAVORITS.CFG -- the global Favorites catalog, up to 60
 *                              entries, each fully self-contained. Fixed
 *                              4-line record per favorite, no keywords:
 *                                type        ("TNFS"/"SDCARD", "" if empty)
 *                                host        (TNFS only, else "")
 *                                port        (TNFS only, else "0")
 *                                full path   (from the TNFS server root,
 *                                             or the complete SD path)
 *                              An empty favorite is simply four empty
 *                              lines. A missing file means "60 empty
 *                              favorites", not an error.
 *
 * RAM discipline: nothing here keeps a config file's content around
 * longer than the one function call that needed it. Each function that
 * needs a file's content reads that ONE file whole, in a single fread()
 * call, into a temporary malloc'd buffer that is freed again before
 * returning -- never a one-line-at-a-time fgetc()/fputc() loop, which on
 * a TNFS-backed GEMDOS drive turned out to cost its own network round
 * trip per small stdio call (see this project's own history). A change
 * to one or two favorites (place/erase/move), or to the Source list, is
 * still applied via a temp file that then replaces the original -- never
 * an in-place edit of a variable-length text record -- just built and
 * written as one whole buffer instead of streamed line by line.
 *
 * Every function here that WRITES shows a GEM alert on a genuine failure
 * (directory/file/temp-file could not be created, write failed, old file
 * could not be removed, temp file could not be renamed into place) and
 * otherwise fails silently and safely -- the running program is never
 * brought down by a storage problem. Every function that only READS never
 * alerts: a missing or short file is normal, not an error, and simply
 * yields empty values.
 */

#include <gem.h>
#include <mint/osbind.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "favcfg.h"

/* "A:\SOME\NESTED\DIR\FLOPPY.CFG\" -- generous for any realistic TOS
 * path; drive+colon+path+"FLOPPY.CFG\"+NUL comfortably fits. */
#define FAVCFG_DIR_MAX  96
/* g_cfg_dir plus "FAVORITS.CFG" or "CONFIG.CFG" (and their own .TMP
 * variants) -- comfortably fits alongside FAVCFG_DIR_MAX. */
#define FAVCFG_PATH_MAX (FAVCFG_DIR_MAX + 16)

/* Worst-case whole-file buffer sizes (see favcfg_read_whole_file()) --
 * every field at its own declared maximum length, which real values
 * essentially never reach, so actual usage is normally a small fraction
 * of this. Allocated on the heap, used for the span of one function
 * call, freed immediately after -- never held beyond that, so this is
 * not a standing RAM cost. Both follow the same "multiply every line by
 * the single biggest field's own length" overestimate this file already
 * used for FAVCFG_CONFIG_BUF_MAX before this rewrite -- simple and safe,
 * not tuned per-field. */
#define FAVCFG_FAVORITES_BUF_MAX (FAVCFG_FAVORITES_MAX_COUNT * 4 * (FAVCFG_PATH_LEN + 2))
#define FAVCFG_CONFIG_BUF_MAX    (MAX_PROFILES * 8 * (PROFILE_SDPATH_LEN + 2))

/* FLOPPY.CFG's own full path, trailing backslash included -- set once by
 * favcfg_init(), read-only from every other function in this file. */
static char g_cfg_dir[FAVCFG_DIR_MAX];

/* ------------------------------------------------------------------
 * Small local helpers -- no dependency on dialog.c's own (all static,
 * private to that file) equivalents.
 * ------------------------------------------------------------------ */

static void favcfg_copy(char *dst, int cap, const char *src)
{
    int n = 0;
    if (!src)
        src = "";
    while (src[n] != '\0' && n < cap - 1) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = '\0';
}

/* Collapses any run of consecutive backslashes in s down to one, in
 * place -- defensive against a Dgetpath() that (unlike TOS's own "no
 * trailing backslash" convention) returns a path WITH one, which would
 * otherwise leave a doubled "\\" wherever favcfg_init() inserts its own
 * separator between that path and "FLOPPY.CFG". */
static void favcfg_collapse_backslashes(char *s)
{
    char *src = s;
    char *dst = s;

    while (*src != '\0') {
        *dst++ = *src;
        if (*src == '\\') {
            while (*(src + 1) == '\\')
                src++;
        }
        src++;
    }
    *dst = '\0';
}

/* Reads the ENTIRE file at path into a freshly malloc'd buffer via ONE
 * fread() call (plus the fopen/fseek/ftell/fclose bookkeeping around it).
 * *out_buf is malloc'd (caller must free() it) and holds exactly
 * *out_len raw bytes -- NOT NUL-terminated, since a config file's own
 * bytes could in principle be anything; callers track the valid range
 * via *out_len and favcfg_next_line()'s own buf_end parameter. Returns 0
 * (*out_buf=NULL, *out_len=0) if the file doesn't exist, is empty, or the
 * allocation failed -- every one of those means "no data", never an
 * error; a genuinely missing/short file is normal, per this file's own
 * header comment. */
static int favcfg_read_whole_file(const char *path, char **out_buf, long *out_len)
{
    FILE *f;
    long len;
    size_t got;

    *out_buf = NULL;
    *out_len = 0;

    f = fopen(path, "rb");
    if (!f)
        return 0;

    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) {
        fclose(f);
        return 0;
    }

    *out_buf = malloc((size_t)len);
    if (!*out_buf) {
        fclose(f);
        return 0;
    }

    got = fread(*out_buf, 1, (size_t)len, f);
    fclose(f);
    *out_len = (long)got;
    return got > 0;
}

/* In-memory equivalent of the line-at-a-time reader this file used to
 * call directly on a FILE* -- "strip \r/\n, bounded, no more data reads
 * as empty" contract, scanning a buffer already fully in RAM (see
 * favcfg_read_whole_file()) instead of issuing its own I/O per line.
 * *cursor advances past the consumed line (including its terminator);
 * buf_end is one-past-the-last valid byte of the whole buffer (NULL/NULL
 * together mean "no buffer at all", e.g. a missing file). Returns 1 if a
 * line was available (even an empty one), 0 if *cursor was already at or
 * past buf_end. */
static int favcfg_next_line(const char **cursor, const char *buf_end, char *buf, int cap)
{
    const char *p = *cursor;
    int n = 0;

    if (!p || p >= buf_end) {
        buf[0] = '\0';
        return 0;
    }

    while (p < buf_end && *p != '\n') {
        if (*p != '\r' && n < cap - 1)
            buf[n++] = *p;
        p++;
    }
    if (p < buf_end)
        p++; /* consume the '\n' */
    buf[n] = '\0';
    *cursor = p;
    return 1;
}

/* Appends s, then "\r\n", to buf at byte offset `offset` (bounded to
 * cap), returning the number of bytes written -- the write-side
 * equivalent of favcfg_next_line(), building a whole file's contents in
 * one in-memory buffer so it can be handed to fwrite() in a single call
 * instead of many small fputs()/fputc() ones. */
static long favcfg_append_line(char *buf, long cap, long offset, const char *s)
{
    long n = 0;

    while (s[n] != '\0' && offset + n < cap - 2) {
        buf[offset + n] = s[n];
        n++;
    }
    buf[offset + n] = '\r';
    buf[offset + n + 1] = '\n';
    return n + 2;
}

static void favcfg_write_error(void)
{
    form_alert(1, "[3][Could not save the|configuration.][OK]");
}

/* Writes the whole buf (len bytes) to out in ONE fwrite() call, then
 * closes out and replaces path with tmp_path -- remove() first, since
 * GEMDOS Frename() is not guaranteed to succeed over an existing
 * destination. On any failure along the way, shows the write-error alert
 * once and cleans up tmp_path; never leaves a half-written path behind
 * (the ORIGINAL path is only ever removed once tmp_path's own contents
 * are confirmed fully and successfully written). Shared tail of every
 * rewrite-via-temp-file operation below. Always closes/frees what it was
 * given, on every path, including failure ones -- callers don't need
 * their own cleanup after calling this. */
static void favcfg_commit(FILE *out, const char *tmp_path, const char *path, const char *buf, long len)
{
    if (fwrite(buf, 1, (size_t)len, out) != (size_t)len) {
        fclose(out);
        remove(tmp_path);
        favcfg_write_error();
        return;
    }
    if (fclose(out) != 0) {
        favcfg_write_error();
        remove(tmp_path);
        return;
    }
    remove(path);
    if (rename(tmp_path, path) != 0)
        favcfg_write_error();
}

/* Copies everything after the LAST '/' in path into out (the whole
 * string if there is no '/' at all) -- the display name for a Favorite,
 * whose own storage now keeps only a single complete path, not a
 * separate name+directory pair. */
static void favcfg_basename(const char *path, char *out, int out_cap)
{
    const char *slash = strrchr(path, '/');
    favcfg_copy(out, out_cap, slash ? slash + 1 : path);
}

/* ------------------------------------------------------------------
 * Paths
 * ------------------------------------------------------------------ */

void favcfg_init(void)
{
    char path[FAVCFG_DIR_MAX];
    short drive;

    path[0] = '\0';
    Dgetpath(path, 0); /* current dir on the current drive: "" at root, else "\SOME\DIR" (leading backslash, no trailing one) */
    drive = Dgetdrv(); /* 0=A, 1=B, 2=C, ... */

    /* path never has a trailing backslash (root comes back as "" and a
     * subdirectory as "\SOME\DIR" -- see the Dgetpath() comment above),
     * so one has to be inserted explicitly here. Without it, a program
     * run from a subdirectory got "\SOMEFLOPPY.CFG\" -- the directory
     * name and "FLOPPY.CFG" glued into one over-long component, which
     * GEMDOS then silently mangled to an 8.3 name (e.g. "FLOPPYFL.CFG")
     * sitting in the wrong place entirely. At root this bug happened to
     * look harmless ("" + "FLOPPY.CFG" needs no separator), which is why
     * it only showed up once FLOPPY.PRG was run from a subdirectory. */
    sprintf(g_cfg_dir, "%c:%s\\FLOPPY.CFG\\", (char)('A' + drive), path);
    favcfg_collapse_backslashes(g_cfg_dir);
}

/* Dcreate() on an already-existing directory also returns an error, and
 * there is no portable way from here to tell that apart from a genuine
 * failure -- so its result is deliberately not checked. The actual
 * fopen()/fclose() of a file inside it (see every caller below) is the
 * real, reliable signal of whether writing is actually possible; that is
 * where favcfg_write_error() gets shown, not here.
 *
 * g_dir_confirmed is set ONLY by favcfg_dir_confirmed() below, which
 * every write call site calls right after its own fopen(tmp_path, "wb")
 * has actually SUCCEEDED -- that success is the real, positive proof the
 * directory exists and is writable, so every later write this session
 * can skip Dcreate() entirely. It is deliberately NOT set just because
 * Dcreate() was *attempted* here: create-style operations are the one
 * category of GEMDOS call this particular TNFS-emulated drive has
 * repeatedly turned out to be slow/unreliable on, and a single transient
 * failure of that one call must not permanently disable saving for the
 * rest of the session -- the next write simply tries Dcreate() again,
 * same as if this were the first write ever. Once real proof exists,
 * though, there's no need to keep re-issuing it on every single write. */
static int g_dir_confirmed = 0;

static void favcfg_ensure_dir(void)
{
    char dir_no_slash[FAVCFG_DIR_MAX];
    int len;

    if (g_dir_confirmed)
        return;

    favcfg_copy(dir_no_slash, sizeof(dir_no_slash), g_cfg_dir);
    len = (int)strlen(dir_no_slash);
    if (len > 0 && dir_no_slash[len - 1] == '\\')
        dir_no_slash[len - 1] = '\0'; /* Dcreate() wants no trailing separator */
    (void)Dcreate(dir_no_slash);
}

static void favcfg_dir_confirmed(void)
{
    g_dir_confirmed = 1;
}

static void favcfg_config_path(char *out)
{
    sprintf(out, "%sCONFIG.CFG", g_cfg_dir);
}

static void favcfg_favorites_path(char *out)
{
    sprintf(out, "%sFAVORITS.CFG", g_cfg_dir);
}

/* ------------------------------------------------------------------
 * CONFIG.CFG -- Browser Sources (the sole local source of truth now,
 * see this file's own header comment)
 * ------------------------------------------------------------------ */

int favcfg_load_config(ProfileConfig *cfg)
{
    char path[FAVCFG_PATH_MAX];
    char *buf;
    long len;
    const char *cursor, *end;
    int i;

    favcfg_config_path(path);
    if (!favcfg_read_whole_file(path, &buf, &len))
        return 0; /* missing -- caller falls back to profile_config_init_defaults() */

    /* Format sniff: this record's own first line is always "EMPTY"/
     * "DISABLED"/"ENABLED". The OLD (pre-architecture-change) CONFIG.CFG
     * format's first line was instead a bare slot number ("1".."8") --
     * a file in that format read against THIS record layout would have
     * every slot's state_str land on some other, unrelated old-format
     * field further down, never once equal to those three keywords, so
     * every slot would silently come back EMPTY -- a real user's
     * configured Sources vanishing with no visible error at all. Treat
     * an unrecognized first line as "not this format" (same as a
     * missing file) rather than silently "succeeding" with data that
     * was never actually read correctly; there is no way to losslessly
     * recover the pre-architecture-change fields (nickname, enabled/
     * disabled state, last_directory) that only ever lived in the now-
     * removed Pico flash profile store, so a clean empty-state fallback
     * is the right outcome here, not a partial/garbled one. */
    {
        char first_line[16];
        const char *peek = buf;
        favcfg_next_line(&peek, buf + len, first_line, sizeof(first_line));
        if (strcmp(first_line, "EMPTY") != 0 && strcmp(first_line, "DISABLED") != 0
            && strcmp(first_line, "ENABLED") != 0) {
            free(buf);
            return 0;
        }
    }

    cursor = buf;
    end = buf + len;

    memset(cfg, 0, sizeof(*cfg));

    for (i = 0; i < MAX_PROFILES; i++) {
        char state_str[16], backend_str[8], port_str[8];
        Profile *p = &cfg->profiles[i];

        favcfg_next_line(&cursor, end, state_str,   sizeof(state_str));
        favcfg_next_line(&cursor, end, backend_str, sizeof(backend_str));
        favcfg_next_line(&cursor, end, p->nickname, sizeof(p->nickname));
        favcfg_next_line(&cursor, end, p->host,     sizeof(p->host));
        favcfg_next_line(&cursor, end, port_str,    sizeof(port_str));
        favcfg_next_line(&cursor, end, p->browser_start_dir, sizeof(p->browser_start_dir));
        favcfg_next_line(&cursor, end, p->sd_path,  sizeof(p->sd_path));
        favcfg_next_line(&cursor, end, p->last_directory, sizeof(p->last_directory));

        if (strcmp(state_str, "ENABLED") == 0)
            p->state = PROFILE_SLOT_ENABLED;
        else if (strcmp(state_str, "DISABLED") == 0)
            p->state = PROFILE_SLOT_DISABLED;
        else
            p->state = PROFILE_SLOT_EMPTY;

        p->backend = (strcmp(backend_str, "SDCARD") == 0) ? PROFILE_BACKEND_SD : PROFILE_BACKEND_TNFS;
        p->port = atoi(port_str);
    }

    /* The active slot is a separate concept (ACTIVE.CFG, below), never
     * stored inside CONFIG.CFG itself -- the caller applies it
     * afterward, same as it already did against the old firmware-fetch
     * path. */
    cfg->active_index = 0;

    free(buf);
    return 1;
}

void favcfg_save_config(const ProfileConfig *cfg)
{
    char path[FAVCFG_PATH_MAX], tmp_path[FAVCFG_PATH_MAX];
    char *buf;
    long len = 0;
    FILE *out;
    int i;

    buf = malloc(FAVCFG_CONFIG_BUF_MAX);
    if (!buf) {
        favcfg_write_error();
        return;
    }

    for (i = 0; i < MAX_PROFILES; i++) {
        const Profile *p = &cfg->profiles[i];
        const char *state_word = (p->state == PROFILE_SLOT_ENABLED) ? "ENABLED"
                                 : (p->state == PROFILE_SLOT_DISABLED) ? "DISABLED" : "EMPTY";

        if (p->state == PROFILE_SLOT_EMPTY) {
            len += favcfg_append_line(buf, FAVCFG_CONFIG_BUF_MAX, len, state_word);
            len += favcfg_append_line(buf, FAVCFG_CONFIG_BUF_MAX, len, "");
            len += favcfg_append_line(buf, FAVCFG_CONFIG_BUF_MAX, len, "");
            len += favcfg_append_line(buf, FAVCFG_CONFIG_BUF_MAX, len, "");
            len += favcfg_append_line(buf, FAVCFG_CONFIG_BUF_MAX, len, "");
            len += favcfg_append_line(buf, FAVCFG_CONFIG_BUF_MAX, len, "");
            len += favcfg_append_line(buf, FAVCFG_CONFIG_BUF_MAX, len, "");
            len += favcfg_append_line(buf, FAVCFG_CONFIG_BUF_MAX, len, "");
        } else {
            int is_sd = (p->backend == PROFILE_BACKEND_SD);
            const char *backend_word = is_sd ? "SDCARD" : "TNFS";
            char port_line[8];

            sprintf(port_line, "%d", is_sd ? 0 : p->port);

            len += favcfg_append_line(buf, FAVCFG_CONFIG_BUF_MAX, len, state_word);
            len += favcfg_append_line(buf, FAVCFG_CONFIG_BUF_MAX, len, backend_word);
            len += favcfg_append_line(buf, FAVCFG_CONFIG_BUF_MAX, len, p->nickname);
            len += favcfg_append_line(buf, FAVCFG_CONFIG_BUF_MAX, len, is_sd ? "" : p->host);
            len += favcfg_append_line(buf, FAVCFG_CONFIG_BUF_MAX, len, port_line);
            len += favcfg_append_line(buf, FAVCFG_CONFIG_BUF_MAX, len, is_sd ? "" : p->browser_start_dir);
            len += favcfg_append_line(buf, FAVCFG_CONFIG_BUF_MAX, len, is_sd ? p->sd_path : "");
            len += favcfg_append_line(buf, FAVCFG_CONFIG_BUF_MAX, len, p->last_directory);
        }
    }

    favcfg_ensure_dir();
    favcfg_config_path(path);
    sprintf(tmp_path, "%sCONFIG.TMP", g_cfg_dir);
    out = fopen(tmp_path, "wb");
    if (!out) {
        free(buf);
        favcfg_write_error();
        return;
    }
    favcfg_dir_confirmed();

    favcfg_commit(out, tmp_path, path, buf, len);
    free(buf);
}

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

static void favcfg_active_path(char *out)
{
    sprintf(out, "%sACTIVE.CFG", g_cfg_dir);
}

int favcfg_read_active_slot(void)
{
    char path[FAVCFG_PATH_MAX];
    char *buf;
    long len;
    char line[8];
    const char *cursor, *end;
    int slot;

    favcfg_active_path(path);
    if (!favcfg_read_whole_file(path, &buf, &len))
        return 0; /* missing -- no remembered choice, not an error */

    cursor = buf;
    end = buf + len;
    favcfg_next_line(&cursor, end, line, sizeof(line));
    free(buf);

    slot = atoi(line);
    return (slot >= 1 && slot <= MAX_PROFILES) ? slot : 0;
}

void favcfg_write_active_slot(int mount_slot)
{
    char path[FAVCFG_PATH_MAX], tmp_path[FAVCFG_PATH_MAX];
    char line[8];
    char buf[16];
    long len = 0;
    FILE *out;

    if (mount_slot < 1 || mount_slot > MAX_PROFILES)
        return;

    sprintf(line, "%d", mount_slot);
    len = favcfg_append_line(buf, (long)sizeof(buf), 0, line);

    favcfg_ensure_dir();
    favcfg_active_path(path);
    sprintf(tmp_path, "%sACTIVE.TMP", g_cfg_dir);
    out = fopen(tmp_path, "wb");
    if (!out) {
        favcfg_write_error();
        return;
    }
    favcfg_dir_confirmed();

    favcfg_commit(out, tmp_path, path, buf, len);
}

/* ------------------------------------------------------------------
 * FAVORITS.CFG (favorites) -- generic whole-file rewrite engine
 * ------------------------------------------------------------------ */

typedef struct {
    int slot;                  /* 1..60, or 0 = unused entry in this array */
    const FavoriteRecord *rec; /* backend == 0 means "clear this slot" -- never NULL */
} FavCfgFavoriteChange;

/* Extracts favorite `favorite`'s full record from an ALREADY-LOADED
 * buffer (buf/len as filled by favcfg_read_whole_file() -- buf may be
 * NULL/len 0 for "no file", read as empty) -- shared by
 * favcfg_read_favorite() (which owns the read itself, for a one-off
 * lookup) and favcfg_move_favorite() (which shares a single read across
 * both sides of the swap AND the rewrite that follows it, rather than
 * opening/reading the same file three separate times for one move --
 * see favcfg_move_favorite()'s own comment for why that matters on a
 * TNFS-backed drive). */
static void favcfg_extract_favorite(const char *buf, long len, int favorite, FavoriteRecord *rec)
{
    const char *cursor, *end;
    char type_str[8], port_str[8];
    int i;

    memset(rec, 0, sizeof(*rec));
    if (favorite < 1 || favorite > FAVCFG_FAVORITES_MAX_COUNT)
        return;

    cursor = buf;
    end = buf ? buf + len : NULL;

    for (i = 1; i < favorite; i++) {
        char skip[FAVCFG_PATH_LEN];
        if (!favcfg_next_line(&cursor, end, skip, sizeof(skip)))
            return; /* file ends before reaching this favorite -- it (and the rest) are empty */
        favcfg_next_line(&cursor, end, skip, sizeof(skip));
        favcfg_next_line(&cursor, end, skip, sizeof(skip));
        favcfg_next_line(&cursor, end, skip, sizeof(skip));
    }

    favcfg_next_line(&cursor, end, type_str, sizeof(type_str));
    favcfg_next_line(&cursor, end, rec->host, sizeof(rec->host));
    favcfg_next_line(&cursor, end, port_str, sizeof(port_str));
    favcfg_next_line(&cursor, end, rec->path, sizeof(rec->path));

    if (strcmp(type_str, "TNFS") == 0)
        rec->backend = PROFILE_BACKEND_TNFS;
    else if (strcmp(type_str, "SDCARD") == 0)
        rec->backend = PROFILE_BACKEND_SD;
    /* else backend stays 0 (empty/unrecognized) from the memset above --
     * host/port/path were still read into rec, but callers must ignore
     * them whenever backend == 0, same contract favcfg.h documents. */

    rec->port = atoi(port_str);
}

/* Core of the whole-file rewrite -- takes an ALREADY-LOADED old buffer
 * (old_buf/old_len as filled by favcfg_read_whole_file(); old_buf may be
 * NULL/0 for "no old file", read as 60 empty favorites) and does NOT
 * free it -- the caller owns that buffer's lifetime, since a caller that
 * already needed the old content for its own purposes
 * (favcfg_move_favorite()) can hand in the SAME buffer here instead of
 * this function reading the file all over again. Builds the replacement
 * content in ONE in-memory buffer, favorite by favorite, substituting
 * `changes` at the matching slot numbers along the way, then writes that
 * whole buffer to a temp file and commits it over the original. */
static void favcfg_rewrite_favorites_buf(const char *old_buf, long old_len,
                                          const FavCfgFavoriteChange *changes, int change_count)
{
    char path[FAVCFG_PATH_MAX], tmp_path[FAVCFG_PATH_MAX];
    const char *cursor, *end;
    char *new_buf;
    long new_len = 0;
    int i;
    FILE *out;

    cursor = old_buf;
    end = old_buf ? old_buf + old_len : NULL;

    new_buf = malloc(FAVCFG_FAVORITES_BUF_MAX);
    if (!new_buf) {
        favcfg_write_error();
        return;
    }

    for (i = 1; i <= FAVCFG_FAVORITES_MAX_COUNT; i++) {
        char old_type[8], old_host[FAVCFG_HOST_LEN], old_port[8], old_path[FAVCFG_PATH_LEN];
        FavoriteRecord use;
        int j;
        int changed = 0;

        /* ALWAYS consume this favorite's four lines from the old content
         * (even when about to override them below), so the cursor stays
         * aligned for every favorite after this one. */
        favcfg_next_line(&cursor, end, old_type, sizeof(old_type));
        favcfg_next_line(&cursor, end, old_host, sizeof(old_host));
        favcfg_next_line(&cursor, end, old_port, sizeof(old_port));
        favcfg_next_line(&cursor, end, old_path, sizeof(old_path));

        for (j = 0; j < change_count; j++) {
            if (changes[j].slot == i) {
                use = *changes[j].rec;
                changed = 1;
                break;
            }
        }

        if (!changed) {
            memset(&use, 0, sizeof(use));
            if (strcmp(old_type, "TNFS") == 0)
                use.backend = PROFILE_BACKEND_TNFS;
            else if (strcmp(old_type, "SDCARD") == 0)
                use.backend = PROFILE_BACKEND_SD;
            favcfg_copy(use.host, sizeof(use.host), old_host);
            use.port = atoi(old_port);
            favcfg_copy(use.path, sizeof(use.path), old_path);
        }

        {
            int is_sd = (use.backend == PROFILE_BACKEND_SD);
            const char *type_word = (use.backend == 0) ? "" : (is_sd ? "SDCARD" : "TNFS");
            char port_line[8];

            sprintf(port_line, "%d", (use.backend != 0 && !is_sd) ? use.port : 0);

            new_len += favcfg_append_line(new_buf, FAVCFG_FAVORITES_BUF_MAX, new_len, type_word);
            new_len += favcfg_append_line(new_buf, FAVCFG_FAVORITES_BUF_MAX, new_len,
                                            (use.backend != 0 && !is_sd) ? use.host : "");
            new_len += favcfg_append_line(new_buf, FAVCFG_FAVORITES_BUF_MAX, new_len, port_line);
            new_len += favcfg_append_line(new_buf, FAVCFG_FAVORITES_BUF_MAX, new_len,
                                            (use.backend != 0) ? use.path : "");
        }
    }

    favcfg_ensure_dir();
    favcfg_favorites_path(path);
    sprintf(tmp_path, "%sFAVORIT.TMP", g_cfg_dir);
    out = fopen(tmp_path, "wb");
    if (!out) {
        free(new_buf);
        favcfg_write_error();
        return;
    }
    favcfg_dir_confirmed();

    favcfg_commit(out, tmp_path, path, new_buf, new_len);
    free(new_buf);
}

/* Reads FAVORITS.CFG whole itself (or treats it as 60 empty favorites if
 * it does not exist yet), then hands off to favcfg_rewrite_favorites_buf()
 * -- used by every write op that doesn't already have the old content in
 * hand (place, erase; favcfg_move_favorite() below reads it itself
 * instead, to share ONE read across both extracting the swap values and
 * this rewrite). */
static void favcfg_rewrite_favorites(const FavCfgFavoriteChange *changes, int change_count)
{
    char path[FAVCFG_PATH_MAX];
    char *old_buf;
    long old_len;

    favcfg_favorites_path(path);
    favcfg_read_whole_file(path, &old_buf, &old_len); /* old_buf NULL if missing -- fine, every favorite then reads as empty */
    favcfg_rewrite_favorites_buf(old_buf, old_len, changes, change_count);
    if (old_buf)
        free(old_buf);
}

void favcfg_write_favorite(int favorite, const FavoriteRecord *rec)
{
    FavCfgFavoriteChange ch;

    if (favorite < 1 || favorite > FAVCFG_FAVORITES_MAX_COUNT)
        return;
    ch.slot = favorite;
    ch.rec = rec;
    favcfg_rewrite_favorites(&ch, 1);
}

void favcfg_erase_favorite(int favorite)
{
    FavoriteRecord empty;
    memset(&empty, 0, sizeof(empty));
    favcfg_write_favorite(favorite, &empty);
}

void favcfg_move_favorite(int from, int to)
{
    char path[FAVCFG_PATH_MAX];
    char *old_buf;
    long old_len;
    FavoriteRecord from_rec, to_rec;
    FavCfgFavoriteChange ch[2];

    if (from == to || from < 1 || from > FAVCFG_FAVORITES_MAX_COUNT
        || to < 1 || to > FAVCFG_FAVORITES_MAX_COUNT)
        return;

    favcfg_favorites_path(path);

    /* ONE read of the file, shared for extracting BOTH sides of the swap
     * and for the rewrite that follows -- opening/reading the same file
     * three separate times in quick succession (from, to, then the
     * rewrite itself) is what the old per-mount-slot favorites move used
     * to do, and is suspected to be what confused the TNFS-emulated
     * drive badly enough to both corrupt the favorites file and leave
     * the drive itself in a bad state afterward (this project's own
     * history). */
    favcfg_read_whole_file(path, &old_buf, &old_len);

    favcfg_extract_favorite(old_buf, old_len, from, &from_rec);
    favcfg_extract_favorite(old_buf, old_len, to, &to_rec);

    /* A true swap, so a non-empty destination is never silently lost,
     * only exchanged. */
    ch[0].slot = to;
    ch[0].rec = &from_rec;
    ch[1].slot = from;
    ch[1].rec = &to_rec;

    favcfg_rewrite_favorites_buf(old_buf, old_len, ch, 2);

    if (old_buf)
        free(old_buf);
}

void favcfg_erase_all_favorites(void)
{
    char path[FAVCFG_PATH_MAX], tmp_path[FAVCFG_PATH_MAX];
    char *buf;
    long len = 0;
    FILE *out;
    int i;

    buf = malloc(FAVCFG_FAVORITES_BUF_MAX);
    if (!buf) {
        favcfg_write_error();
        return;
    }

    for (i = 0; i < FAVCFG_FAVORITES_MAX_COUNT; i++) {
        len += favcfg_append_line(buf, FAVCFG_FAVORITES_BUF_MAX, len, "");
        len += favcfg_append_line(buf, FAVCFG_FAVORITES_BUF_MAX, len, "");
        len += favcfg_append_line(buf, FAVCFG_FAVORITES_BUF_MAX, len, "");
        len += favcfg_append_line(buf, FAVCFG_FAVORITES_BUF_MAX, len, "");
    }

    favcfg_ensure_dir();
    favcfg_favorites_path(path);
    sprintf(tmp_path, "%sFAVORIT.TMP", g_cfg_dir);
    out = fopen(tmp_path, "wb");
    if (!out) {
        free(buf);
        favcfg_write_error();
        return;
    }
    favcfg_dir_confirmed();

    favcfg_commit(out, tmp_path, path, buf, len);
    free(buf);
}

/* ------------------------------------------------------------------
 * FAVORITS.CFG -- reads
 * ------------------------------------------------------------------ */

void favcfg_read_favorite(int favorite, FavoriteRecord *rec)
{
    char path[FAVCFG_PATH_MAX];
    char *buf;
    long len;

    memset(rec, 0, sizeof(*rec));
    favcfg_favorites_path(path);
    favcfg_read_whole_file(path, &buf, &len); /* buf NULL if missing -- favcfg_extract_favorite() treats that as empty */
    favcfg_extract_favorite(buf, len, favorite, rec);
    if (buf)
        free(buf);
}

void favcfg_read_favorite_name(int favorite, char *out_name, int out_name_cap)
{
    FavoriteRecord rec;

    favcfg_read_favorite(favorite, &rec);
    if (rec.backend == 0) {
        out_name[0] = '\0';
        return;
    }
    favcfg_basename(rec.path, out_name, out_name_cap);
}

/* One open/read of FAVORITS.CFG for the whole batch, then a single linear
 * scan through it -- unlike calling favcfg_read_favorite_name() count
 * times (each of which re-opens the file AND re-skips from favorite 1
 * every time), this walks forward through the buffer exactly once. Built
 * for dialog.c's fa_refresh_rows(), which used to cost FA_ROWS (15)
 * separate file opens to build one page -- on a TNFS-backed drive, each
 * open has its own real network cost, which is what made building a page
 * of favorites visibly slow. */
void favcfg_read_page_favorite_names(int first_favorite, int count,
                                       char out_names[][FAVCFG_NAME_LEN])
{
    char path[FAVCFG_PATH_MAX];
    char *buf;
    long len;
    const char *cursor, *end;
    int i;

    for (i = 0; i < count; i++)
        out_names[i][0] = '\0';

    if (first_favorite < 1 || count <= 0)
        return;

    favcfg_favorites_path(path);
    if (!favcfg_read_whole_file(path, &buf, &len))
        return; /* no favorites file yet -- every favorite in the batch reads as empty */

    cursor = buf;
    end = buf + len;

    for (i = 1; i < first_favorite; i++) {
        char skip[FAVCFG_PATH_LEN];
        if (!favcfg_next_line(&cursor, end, skip, sizeof(skip))) {
            free(buf);
            return; /* file ends before reaching first_favorite -- the whole batch is empty */
        }
        favcfg_next_line(&cursor, end, skip, sizeof(skip));
        favcfg_next_line(&cursor, end, skip, sizeof(skip));
        favcfg_next_line(&cursor, end, skip, sizeof(skip));
    }

    for (i = 0; i < count; i++) {
        char type_str[8], host_skip[FAVCFG_HOST_LEN], port_skip[8], path_buf[FAVCFG_PATH_LEN];

        if (!favcfg_next_line(&cursor, end, type_str, sizeof(type_str)))
            break; /* file ends -- remaining out_names[] entries stay "" (already cleared above) */
        favcfg_next_line(&cursor, end, host_skip, sizeof(host_skip));
        favcfg_next_line(&cursor, end, port_skip, sizeof(port_skip));
        favcfg_next_line(&cursor, end, path_buf, sizeof(path_buf));

        if (type_str[0] != '\0')
            favcfg_basename(path_buf, out_names[i], FAVCFG_NAME_LEN);
    }

    free(buf);
}
