/*
 * FLOPPY.PRG favorites persistence.
 *
 * Deliberately 1980s-plain: plain text, one value per line, a STRICT
 * fixed line order per record, no keywords, no headers, no magic values,
 * no version fields, no CRC, no binary records. Per this project's own
 * task brief for this step -- see that brief for the full rationale; not
 * repeated here.
 *
 * Layout on disk, next to FLOPPY.PRG itself (see favcfg_init()):
 *
 *   FLOPPY.CFG\CONFIG.CFG   -- snapshot of the 8 mount definitions last
 *                              seen, so a later mount-slot reuse (a
 *                              different server/path/port/type moved into
 *                              the same slot number) can be detected and
 *                              that slot's stale favorites cleared. Fixed
 *                              5-line record per slot, 8 slots, no
 *                              keywords:
 *                                slot number
 *                                mount type ("TNFS" / "SDCARD" / "" empty)
 *                                server name/IP (TNFS only, else "")
 *                                mount path / SD directory
 *                                port (TNFS only, else "0")
 *
 *   FLOPPY.CFG\MOUNTn.CFG   -- one file per mount slot (1..8), the 60
 *                              favorites belonging to whatever is
 *                              currently in that slot. Fixed 2-line
 *                              record per favorite, 60 favorites, no
 *                              keywords:
 *                                filename
 *                                directory
 *                              An empty favorite is simply two empty
 *                              lines. Missing files (mount never used
 *                              yet, or FLOPPY.CFG itself missing) mean
 *                              "60 empty favorites", not an error.
 *
 * RAM discipline: nothing here keeps a config file's content around
 * longer than the one function call that needed it -- never all 8 mount
 * definitions, never all 60 favorites of a mount, and definitely never
 * 8x60, held in memory across calls. What DID change (see
 * favcfg_read_whole_file()'s own comment): each function that needs a
 * file's content now reads that ONE file whole, in a single fread() call,
 * into a temporary malloc'd buffer that is freed again before returning --
 * rather than the original design's one-line-at-a-time fgetc()/fputc()
 * loop. On a TNFS-backed GEMDOS drive, each of those small stdio calls
 * turned out to cost its own network round trip, making a 60-favorite
 * lookup or rewrite dozens to hundreds of round trips instead of one or
 * two -- slow reads, and writes slow enough to effectively never
 * complete. A change to one or two favorites (place/erase/move) is still
 * applied via a temp file (MOUNTn.TMP) that then replaces the original --
 * never an in-place edit of a variable-length text record -- just built
 * and written as one whole buffer instead of streamed line by line.
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

#define FAVCFG_FAVORITES_PER_MOUNT 60

/* "A:\SOME\NESTED\DIR\FLOPPY.CFG\" -- generous for any realistic TOS
 * path; drive+colon+path+"FLOPPY.CFG\"+NUL comfortably fits. */
#define FAVCFG_DIR_MAX  96
/* g_cfg_dir plus "MOUNT8.CFG" or "CONFIG.CFG" (and their own .TMP
 * variants) -- comfortably fits alongside FAVCFG_DIR_MAX. */
#define FAVCFG_PATH_MAX (FAVCFG_DIR_MAX + 16)

/* Worst-case whole-file buffer sizes (see favcfg_read_whole_file()) --
 * every favorite/field at its own declared maximum length, which real
 * filenames/paths/hostnames essentially never reach, so actual usage is
 * normally a small fraction of this. Allocated on the heap, used for the
 * span of one function call, freed immediately after -- never held
 * beyond that, so this is not a standing RAM cost. */
#define FAVCFG_MOUNT_BUF_MAX  (FAVCFG_FAVORITES_PER_MOUNT * 2 * (FAVCFG_NAME_LEN + 2))
#define FAVCFG_CONFIG_BUF_MAX (MAX_PROFILES * 5 * (PROFILE_SDPATH_LEN + 2))

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
 * fread() call (plus the fopen/fseek/ftell/fclose bookkeeping around it),
 * instead of the many separate one-line-at-a-time reads this file used to
 * do -- see this file's own header comment on why that mattered. *out_buf
 * is malloc'd (caller must free() it) and holds exactly *out_len raw
 * bytes -- NOT NUL-terminated, since a config file's own bytes could in
 * principle be anything; callers track the valid range via *out_len and
 * favcfg_next_line()'s own buf_end parameter. Returns 0 (*out_buf=NULL,
 * *out_len=0) if the file doesn't exist, is empty, or the allocation
 * failed -- every one of those means "no data", never an error; a
 * genuinely missing/short file is normal, per this file's own header
 * comment. */
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
 * call directly on a FILE* -- same "strip \r/\n, bounded, no more data
 * reads as empty" contract, but scans a buffer already fully in RAM (see
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
    form_alert(1, "[3][Could not save the|favorites configuration.][OK]");
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
 * repeatedly turned out to be slow/unreliable on (see this project's own
 * history: the original Dcreate/Fcreate hang, and the firmware's own
 * bounded-retry fix for exactly this kind of call), and a single
 * transient failure of that one call must not permanently disable saving
 * for the rest of the session -- the next write simply tries Dcreate()
 * again, same as if this were the first write ever. Once real proof
 * exists, though, there's no need to keep re-issuing it on every single
 * favorite save. */
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

/* mount_slot is 1..8 (matches the profile-config slot number the user
 * sees, not a 0-based array index). Returns 0 (out left untouched) for
 * an out-of-range slot -- e.g. no active profile at all (caller would
 * pass cfg->active_index + 1 = 0 in that case). */
static int favcfg_mount_path(int mount_slot, char *out)
{
    if (mount_slot < 1 || mount_slot > MAX_PROFILES)
        return 0;
    sprintf(out, "%sMOUNT%d.CFG", g_cfg_dir, mount_slot);
    return 1;
}

static void favcfg_config_path(char *out)
{
    sprintf(out, "%sCONFIG.CFG", g_cfg_dir);
}

/* ------------------------------------------------------------------
 * MOUNTn.CFG (favorites) -- generic whole-file rewrite engine
 * ------------------------------------------------------------------ */

typedef struct {
    int slot;          /* 1..60, or 0 = unused entry in this array */
    const char *name;  /* NULL treated as "" */
    const char *dir;   /* NULL treated as "" */
} FavCfgChange;

/* Extracts favorite `favorite`'s name+dir from an ALREADY-LOADED buffer
 * (buf/len as filled by favcfg_read_whole_file() -- buf may be NULL/len 0
 * for "no file", read as empty) -- shared by favcfg_read_entry() (which
 * owns the read itself, for a one-off lookup) and favcfg_move_entry()
 * (which shares a single read across both sides of the swap AND the
 * rewrite that follows it, rather than opening/reading the same file
 * three separate times for one move). */
static void favcfg_extract_entry(const char *buf, long len, int favorite,
                                  char *out_name, int out_name_cap,
                                  char *out_dir, int out_dir_cap)
{
    const char *cursor, *end;
    int i;

    out_name[0] = '\0';
    out_dir[0] = '\0';

    if (favorite < 1 || favorite > FAVCFG_FAVORITES_PER_MOUNT)
        return;

    cursor = buf;
    end = buf ? buf + len : NULL;

    for (i = 1; i < favorite; i++) {
        char skip[FAVCFG_NAME_LEN];
        if (!favcfg_next_line(&cursor, end, skip, sizeof(skip)))
            return; /* file ends before reaching this favorite -- it (and the rest) are empty */
        favcfg_next_line(&cursor, end, skip, sizeof(skip));
    }
    favcfg_next_line(&cursor, end, out_name, out_name_cap);
    favcfg_next_line(&cursor, end, out_dir, out_dir_cap);
}

/* Core of the whole-file rewrite -- takes an ALREADY-LOADED old buffer
 * (old_buf/old_len as filled by favcfg_read_whole_file(); old_buf may be
 * NULL/0 for "no old file", read as 60 empty favorites) and does NOT
 * free it -- the caller owns that buffer's lifetime, since a caller that
 * already needed the old content for its own purposes (favcfg_move_entry())
 * can hand in the SAME buffer here instead of this function reading the
 * file all over again. Builds the replacement content in ONE in-memory
 * buffer, favorite by favorite, substituting `changes` at the matching
 * slot numbers along the way, then writes that whole buffer to a temp
 * file and commits it over the original. */
static void favcfg_rewrite_mount_buf(int mount_slot, const char *old_buf, long old_len,
                                      const FavCfgChange *changes, int change_count)
{
    char path[FAVCFG_PATH_MAX], tmp_path[FAVCFG_PATH_MAX];
    const char *cursor, *end;
    char *new_buf;
    long new_len = 0;
    int i;
    FILE *out;

    if (!favcfg_mount_path(mount_slot, path))
        return;

    cursor = old_buf;
    end = old_buf ? old_buf + old_len : NULL;

    new_buf = malloc(FAVCFG_MOUNT_BUF_MAX);
    if (!new_buf) {
        favcfg_write_error();
        return;
    }

    for (i = 1; i <= FAVCFG_FAVORITES_PER_MOUNT; i++) {
        char name_buf[FAVCFG_NAME_LEN], dir_buf[FAVCFG_DIR_LEN];
        const char *use_name, *use_dir;
        int have_name, have_dir;
        int j;

        /* ALWAYS consume this favorite's two lines from the old content
         * (even when about to override them below), so the cursor stays
         * aligned for every favorite after this one. */
        have_name = favcfg_next_line(&cursor, end, name_buf, sizeof(name_buf));
        have_dir  = favcfg_next_line(&cursor, end, dir_buf, sizeof(dir_buf));
        use_name = have_name ? name_buf : "";
        use_dir  = have_dir ? dir_buf : "";

        for (j = 0; j < change_count; j++) {
            if (changes[j].slot == i) {
                use_name = changes[j].name ? changes[j].name : "";
                use_dir  = changes[j].dir ? changes[j].dir : "";
                break;
            }
        }

        new_len += favcfg_append_line(new_buf, FAVCFG_MOUNT_BUF_MAX, new_len, use_name);
        new_len += favcfg_append_line(new_buf, FAVCFG_MOUNT_BUF_MAX, new_len, use_dir);
    }

    favcfg_ensure_dir();
    sprintf(tmp_path, "%sMOUNT%d.TMP", g_cfg_dir, mount_slot);
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

/* Reads MOUNTn.CFG whole itself (or treats it as 60 empty favorites if it
 * does not exist yet), then hands off to favcfg_rewrite_mount_buf() --
 * used by every write op that doesn't already have the old content in
 * hand (place, erase; favcfg_move_entry() below reads it itself instead,
 * to share ONE read across both extracting the swap values and this
 * rewrite). */
static void favcfg_rewrite_mount(int mount_slot, const FavCfgChange *changes, int change_count)
{
    char path[FAVCFG_PATH_MAX];
    char *old_buf;
    long old_len;

    if (!favcfg_mount_path(mount_slot, path))
        return;

    favcfg_read_whole_file(path, &old_buf, &old_len); /* old_buf NULL if missing -- fine, every favorite then reads as empty */
    favcfg_rewrite_mount_buf(mount_slot, old_buf, old_len, changes, change_count);
    if (old_buf)
        free(old_buf);
}

void favcfg_write_entry(int mount_slot, int favorite, const char *name, const char *dir)
{
    FavCfgChange ch;

    if (favorite < 1 || favorite > FAVCFG_FAVORITES_PER_MOUNT)
        return;
    ch.slot = favorite;
    ch.name = name;
    ch.dir = dir;
    favcfg_rewrite_mount(mount_slot, &ch, 1);
}

void favcfg_erase_entry(int mount_slot, int favorite)
{
    favcfg_write_entry(mount_slot, favorite, "", "");
}

void favcfg_move_entry(int mount_slot, int from, int to)
{
    char path[FAVCFG_PATH_MAX];
    char *old_buf;
    long old_len;
    char from_name[FAVCFG_NAME_LEN], from_dir[FAVCFG_DIR_LEN];
    char to_name[FAVCFG_NAME_LEN], to_dir[FAVCFG_DIR_LEN];
    FavCfgChange ch[2];

    if (from == to || from < 1 || from > FAVCFG_FAVORITES_PER_MOUNT
        || to < 1 || to > FAVCFG_FAVORITES_PER_MOUNT)
        return;

    if (!favcfg_mount_path(mount_slot, path))
        return;

    /* ONE read of the file, shared for extracting BOTH sides of the swap
     * and for the rewrite that follows -- opening/reading the same file
     * three separate times in quick succession (from, to, then the
     * rewrite itself) is what this used to do, and is suspected to be
     * what confused the TNFS-emulated drive badly enough to both corrupt
     * MOUNTn.CFG (every favorite except the two swapped ones coming back
     * empty) and leave the drive itself in a bad state afterward. */
    favcfg_read_whole_file(path, &old_buf, &old_len);

    favcfg_extract_entry(old_buf, old_len, from, from_name, sizeof(from_name), from_dir, sizeof(from_dir));
    favcfg_extract_entry(old_buf, old_len, to, to_name, sizeof(to_name), to_dir, sizeof(to_dir));

    /* A true swap, so a non-empty destination is never silently lost,
     * only exchanged. */
    ch[0].slot = to;
    ch[0].name = from_name;
    ch[0].dir = from_dir;
    ch[1].slot = from;
    ch[1].name = to_name;
    ch[1].dir = to_dir;

    favcfg_rewrite_mount_buf(mount_slot, old_buf, old_len, ch, 2);

    if (old_buf)
        free(old_buf);
}

void favcfg_erase_all(int mount_slot)
{
    char path[FAVCFG_PATH_MAX], tmp_path[FAVCFG_PATH_MAX];
    char buf[FAVCFG_FAVORITES_PER_MOUNT * 4]; /* 60 * ("\r\n" + "\r\n") = 240 bytes -- small enough for the stack, no old content to read */
    long len = 0;
    FILE *out;
    int i;

    if (!favcfg_mount_path(mount_slot, path))
        return;

    for (i = 0; i < FAVCFG_FAVORITES_PER_MOUNT; i++) {
        len += favcfg_append_line(buf, (long)sizeof(buf), len, "");
        len += favcfg_append_line(buf, (long)sizeof(buf), len, "");
    }

    favcfg_ensure_dir();
    sprintf(tmp_path, "%sMOUNT%d.TMP", g_cfg_dir, mount_slot);
    out = fopen(tmp_path, "wb");
    if (!out) {
        favcfg_write_error();
        return;
    }
    favcfg_dir_confirmed();

    favcfg_commit(out, tmp_path, path, buf, len);
}

/* ------------------------------------------------------------------
 * MOUNTn.CFG -- reads
 * ------------------------------------------------------------------ */

void favcfg_read_entry(int mount_slot, int favorite,
                        char *out_name, int out_name_cap,
                        char *out_dir, int out_dir_cap)
{
    char path[FAVCFG_PATH_MAX];
    char *buf;
    long len;

    out_name[0] = '\0';
    out_dir[0] = '\0';

    if (!favcfg_mount_path(mount_slot, path))
        return;

    favcfg_read_whole_file(path, &buf, &len); /* buf NULL if missing -- favcfg_extract_entry() treats that as empty */
    favcfg_extract_entry(buf, len, favorite, out_name, out_name_cap, out_dir, out_dir_cap);
    if (buf)
        free(buf);
}

void favcfg_read_name(int mount_slot, int favorite, char *out_name, int out_name_cap)
{
    char dir_buf[FAVCFG_DIR_LEN];
    favcfg_read_entry(mount_slot, favorite, out_name, out_name_cap, dir_buf, sizeof(dir_buf));
}

/* One open/read of MOUNTn.CFG for the whole batch, then a single linear
 * scan through it -- unlike calling favcfg_read_name() count times (each
 * of which re-opens the file AND re-skips from favorite 1 every time),
 * this walks forward through the buffer exactly once. Built for
 * dialog.c's fa_refresh_rows(), which used to cost FA_ROWS (15) separate
 * file opens to build one page -- on a TNFS-backed drive, each open has
 * its own real network cost, which is what made building a page of
 * favorites visibly slow. */
void favcfg_read_page_names(int mount_slot, int first_favorite, int count,
                             char out_names[][FAVCFG_NAME_LEN])
{
    char path[FAVCFG_PATH_MAX];
    char *buf;
    long len;
    const char *cursor, *end;
    int i;

    for (i = 0; i < count; i++)
        out_names[i][0] = '\0';

    if (!favcfg_mount_path(mount_slot, path))
        return;
    if (first_favorite < 1 || count <= 0)
        return;

    if (!favcfg_read_whole_file(path, &buf, &len))
        return; /* no favorites file yet -- every favorite in the batch reads as empty */

    cursor = buf;
    end = buf + len;

    for (i = 1; i < first_favorite; i++) {
        char skip[FAVCFG_NAME_LEN];
        if (!favcfg_next_line(&cursor, end, skip, sizeof(skip))) {
            free(buf);
            return; /* file ends before reaching first_favorite -- the whole batch is empty */
        }
        favcfg_next_line(&cursor, end, skip, sizeof(skip));
    }

    for (i = 0; i < count; i++) {
        char dir_skip[FAVCFG_DIR_LEN];
        if (!favcfg_next_line(&cursor, end, out_names[i], FAVCFG_NAME_LEN))
            break; /* file ends -- remaining out_names[] entries stay "" (already cleared above) */
        favcfg_next_line(&cursor, end, dir_skip, sizeof(dir_skip));
    }

    free(buf);
}

/* ------------------------------------------------------------------
 * CONFIG.CFG -- mount-change detection
 * ------------------------------------------------------------------ */

/* Derives CONFIG.CFG's own 4 comparable/storable fields from a live
 * Profile record. An unconfigured (EMPTY) slot derives to all-blank/0,
 * matching exactly what a slot CONFIG.CFG has never recorded anything
 * for also reads as (see favcfg_next_line()'s own "no buffer" behavior)
 * -- so an all-empty ProfileConfig against a missing CONFIG.CFG compares
 * as "everything matches" and writes nothing, exactly as a true first
 * run should. */
static void favcfg_profile_fields(const Profile *p, char *type, int type_cap,
                                   char *server, int server_cap,
                                   char *path, int path_cap, int *port)
{
    if (!profile_slot_is_configured(p)) {
        type[0] = '\0';
        server[0] = '\0';
        path[0] = '\0';
        *port = 0;
        return;
    }
    if (p->backend == PROFILE_BACKEND_SD) {
        favcfg_copy(type, type_cap, "SDCARD");
        server[0] = '\0';
        favcfg_copy(path, path_cap, p->sd_path);
        *port = 0;
    } else {
        favcfg_copy(type, type_cap, "TNFS");
        favcfg_copy(server, server_cap, p->host);
        favcfg_copy(path, path_cap, p->mount_path);
        *port = p->port;
    }
}

/* Rewrites CONFIG.CFG wholesale from cfg's own current 8 profiles -- an
 * unchanged slot's freshly-derived record is, by definition, identical to
 * what was already stored (that is what "unchanged" means), so there is
 * no need to preserve the OLD file's own bytes for those slots; every
 * slot is simply regenerated fresh, built into one in-memory buffer and
 * written in a single fwrite(). Only called when at least one slot
 * actually changed (see favcfg_startup_validate()) -- an all-matching
 * comparison never touches the file at all. */
static void favcfg_rewrite_config(const ProfileConfig *cfg)
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
        char type[8], server[PROFILE_HOST_LEN], path_buf[PROFILE_SDPATH_LEN];
        char line[16];
        int port;

        favcfg_profile_fields(&cfg->profiles[i], type, sizeof(type),
                               server, sizeof(server), path_buf, sizeof(path_buf), &port);

        sprintf(line, "%d", i + 1);
        len += favcfg_append_line(buf, FAVCFG_CONFIG_BUF_MAX, len, line);
        len += favcfg_append_line(buf, FAVCFG_CONFIG_BUF_MAX, len, type);
        len += favcfg_append_line(buf, FAVCFG_CONFIG_BUF_MAX, len, server);
        len += favcfg_append_line(buf, FAVCFG_CONFIG_BUF_MAX, len, path_buf);
        sprintf(line, "%d", port);
        len += favcfg_append_line(buf, FAVCFG_CONFIG_BUF_MAX, len, line);
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

void favcfg_startup_validate(const ProfileConfig *cfg)
{
    char path[FAVCFG_PATH_MAX];
    char *buf;
    long len;
    const char *cursor, *end;
    int i;
    int any_mismatch = 0;

    favcfg_config_path(path);
    favcfg_read_whole_file(path, &buf, &len); /* buf NULL if missing -- every slot then compares against "no prior record" */
    cursor = buf;
    end = buf ? buf + len : NULL;

    for (i = 0; i < MAX_PROFILES; i++) {
        char cur_type[8], cur_server[PROFILE_HOST_LEN], cur_path[PROFILE_SDPATH_LEN];
        char old_num[8], old_type[8], old_server[PROFILE_HOST_LEN], old_path[PROFILE_SDPATH_LEN], old_port_str[8];
        int cur_port, old_port;

        favcfg_profile_fields(&cfg->profiles[i], cur_type, sizeof(cur_type),
                               cur_server, sizeof(cur_server), cur_path, sizeof(cur_path), &cur_port);

        /* The stored slot number itself is read (to stay aligned with the
         * fixed 5-line record) but never compared -- position in the
         * file already identifies which mount this is, per this
         * project's own task brief. */
        favcfg_next_line(&cursor, end, old_num, sizeof(old_num));
        favcfg_next_line(&cursor, end, old_type, sizeof(old_type));
        favcfg_next_line(&cursor, end, old_server, sizeof(old_server));
        favcfg_next_line(&cursor, end, old_path, sizeof(old_path));
        favcfg_next_line(&cursor, end, old_port_str, sizeof(old_port_str));
        old_port = atoi(old_port_str);

        if (strcmp(cur_type, old_type) != 0
            || strcmp(cur_server, old_server) != 0
            || strcmp(cur_path, old_path) != 0
            || cur_port != old_port) {
            any_mismatch = 1;
            favcfg_erase_all(i + 1);
        }
    }

    if (buf)
        free(buf);

    if (any_mismatch)
        favcfg_rewrite_config(cfg);
}

/* ------------------------------------------------------------------
 * ACTIVE.CFG -- last-selected source slot
 *
 * One bare line, the mount slot number (1..8) -- same "no keywords, no
 * header" plain-text style as every other file here. Deliberately
 * separate from CONFIG.CFG rather than a 9th line tacked onto it: that
 * file's own 8-slot, 5-line-per-slot layout is fixed, and this value
 * has nothing to do with any one slot's own definition. Exists because
 * picking a source via [Source] only ever changes cfg->active_index in
 * RAM -- the firmware's own active-profile flash write only happens on
 * an explicit profile Save (see perform_save()'s own comment in
 * dialog.c), so without this the choice never survived a restart.
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
