/*
 * FLOPPY.PRG server-profile protocol: GET_CONFIG_INFO (0x041D),
 * GET_PROFILE (0x041E), SET_PROFILE (0x041F), DELETE_PROFILE (0x0420),
 * SET_ACTIVE_PROFILE (0x0421), SAVE_PROFILES (0x0422).
 *
 * Protocol cross-checked against (read-only references, not modified):
 *   SideTNFS-Firmware/romemul/include/commands.h     (command codes, floppyemu branch)
 *   SideTNFS-Firmware/romemul/include/gemdrvemul.h   (offsets, walked from
 *     GEMDRVEMUL_FLOPPY_CONFIG, floppyemu branch)
 *   SideTNFS-Firmware/romemul/include/sidetnfs_floppy_config.h (status/state
 *     enum values, field lengths, floppyemu branch)
 *
 * The low-level ROM3 transport primitives below (rom3_read*, the random-
 * token handshake, send_param16/32, send/read_string_field) are copied
 * verbatim from SideTNFS-Config's own sidetnfs_probe.c -- proven, generic
 * plumbing with no SideTNFS-drive-specific meaning (see this project's
 * RESEARCH-STEP0.md section 3.8, reuse list A). Only the command IDs,
 * offsets, and wire structs below are new.
 *
 * Byte-order summary (same reasoning as SideTNFS-Config's sidetnfs_probe.c,
 * cross-checked against the new firmware code's own use of the identical
 * WRITE_AND_SWAP_LONGWORD/WRITE_WORD/CHANGE_ENDIANESS_BLOCK16/
 * COPY_AND_CHANGE_ENDIANESS_BLOCK16 macros -- not a new mechanism):
 *   - uint32_t fields (WRITE_AND_SWAP_LONGWORD on the Pico side): a plain
 *     32-bit volatile read is correct as-is.
 *   - uint16_t fields (WRITE_WORD, no swap): a plain 16-bit volatile read
 *     is correct.
 *   - char[] fields, Pico->Atari (GET_PROFILE): byte-copy +
 *     CHANGE_ENDIANESS_BLOCK16 on the Pico side -- read byte-for-byte in
 *     address order on the Atari side, no unswap needed.
 *   - char[] fields, Atari->Pico (SET_PROFILE): sent as address-encoded
 *     reads where each 16-bit "address" is two RAW consecutive source
 *     bytes packed big-endian ((b0<<8)|b1) -- exactly what
 *     send_sync_write_command_to_sidecart's even-address word-copy loop
 *     does. The firmware's COPY_AND_CHANGE_ENDIANESS_BLOCK16 un-swaps this
 *     back into the original byte order on receipt.
 *   - uint16_t request params (SET_PROFILE's state/backend/port): sent as ONE
 *     address-encoded read of the raw value, matching
 *     GET_PAYLOAD_PARAM16(payload) = payload[0].
 *   - uint32_t request params (index): sent as two address-encoded reads
 *     (low word, then high word), matching
 *     GET_PAYLOAD_PARAM32(payload) = (payload[1]<<16)|payload[0].
 */

#include <mint/osbind.h>
#include "floppy_probe.h"

#define ROM3_BASE            0xFB0000UL
#define ROM3_PROTOCOL_HEADER 0xABCDUL /* CMD_MAGIC_NUMBER, gemdrive.s */

#define CMD_FLOPPY_GET_CONFIG_INFO   0x041DUL
#define CMD_FLOPPY_GET_PROFILE       0x041EUL
#define CMD_FLOPPY_SET_PROFILE       0x041FUL
#define CMD_FLOPPY_DELETE_PROFILE    0x0420UL
#define CMD_FLOPPY_SET_ACTIVE_PROFILE 0x0421UL
#define CMD_FLOPPY_SAVE_PROFILES     0x0422UL

#define RANDOM_TOKEN_OFFSET      0x0000UL /* echoed token, polled after completion */
#define RANDOM_TOKEN_SEED_OFFSET 0x0004UL /* Pico-published seed, read before sending */

/* GEMDRVEMUL_FLOPPY_CONFIG = 0x4598 -- immediately after the existing
 * GEMDRVEMUL_SIDETNFS_UPDATE (CHECK_UPDATE) block, walked by hand from
 * gemdrvemul.h (floppyemu branch) and re-confirmed against the firmware's
 * own compiled offset chain -- not independently guessed. */
#define RESP_CONFIG_VERSION_OFFSET      0x4598UL
#define RESP_CONFIG_MAX_PROFILES_OFFSET 0x459CUL
#define RESP_CONFIG_PROFILE_COUNT_OFFSET 0x45A0UL
#define RESP_CONFIG_ACTIVE_INDEX_OFFSET 0x45A4UL
#define RESP_CONFIG_STATUS_OFFSET       0x45A8UL

/* GEMDRVEMUL_FLOPPY_PROFILE = CONFIG_STATUS + 4 = 0x45AC. Field order and
 * offsets re-walked by hand from gemdrvemul.h (floppyemu branch) after the
 * SD-backend addition -- state/backend/port are now three separate
 * uint16_t fields (was state/port only), and last_directory now comes
 * BEFORE host/mount_path/sd_path (was after) -- see that header's own
 * GEMDRVEMUL_FLOPPY_PROFILE_* chain, not independently guessed. */
#define PROFILE_STATUS_OFFSET   0x45ACUL /* uint32_t */
#define PROFILE_STATE_OFFSET    0x45B0UL /* uint16_t */
#define PROFILE_BACKEND_OFFSET  0x45B2UL /* uint16_t */
#define PROFILE_PORT_OFFSET     0x45B4UL /* uint16_t */
#define PROFILE_NICKNAME_OFFSET 0x45B6UL /* char[24] */
#define PROFILE_LAST_DIRECTORY_OFFSET 0x45CEUL /* char[256] */
#define PROFILE_HOST_OFFSET     0x46CEUL /* char[64] -- TNFS only */
#define PROFILE_MOUNT_PATH_OFFSET 0x470EUL /* char[32] -- TNFS only */
#define PROFILE_SD_PATH_OFFSET  0x472EUL /* char[256] -- SD only, block ends 0x482E */

/* SET_PROFILE request payload size, excluding the 4-byte token:
 * index(4) + state+backend+port(2 each=6) + strings
 * (24+256+64+32+256=632) = 642 bytes. */
#define SET_PROFILE_PAYLOAD_BYTES \
    (4UL + 2UL*3UL + (unsigned long)FLOPPY_NICKNAME_LEN + (unsigned long)FLOPPY_LASTDIR_LEN + \
     (unsigned long)FLOPPY_HOST_LEN + (unsigned long)FLOPPY_MOUNTPATH_LEN + (unsigned long)FLOPPY_SDPATH_LEN)

#define PROBE_TIMEOUT_SEC       2
#define SAVE_PROFILES_TIMEOUT_SEC 5 /* SAVE_PROFILES does real flash erase+program */
#define PAL_VBLS_PER_SEC        50 /* assuming PAL system, matches SideTNFS-Config's sidetnfs_probe.c */

static unsigned char rom3_read(unsigned long offset)
{
    return *((volatile unsigned char *)(ROM3_BASE + offset));
}

static unsigned short rom3_read_word(unsigned long offset)
{
    return *((volatile unsigned short *)(ROM3_BASE + offset));
}

static unsigned long rom3_read_long(unsigned long offset)
{
    return *((volatile unsigned long *)(ROM3_BASE + offset));
}

/* Bounded Vsync() poll for the firmware to echo `seed` back at
 * RANDOM_TOKEN_OFFSET. Returns 1 on match, 0 on timeout. */
static int wait_for_token(unsigned long seed, int timeout_sec)
{
    unsigned long echoed;
    long budget;

    budget = (long)timeout_sec * PAL_VBLS_PER_SEC;
    echoed = ~seed; /* force a mismatch before the first check */
    while (budget > 0 && echoed != seed) {
        Vsync();
        echoed = rom3_read_long(RANDOM_TOKEN_OFFSET);
        budget--;
    }
    return echoed == seed;
}

/* Seed capture + header/command/payload-size trigger reads + the token
 * itself (low word, then high word) -- identical shape for every command,
 * only `command` and the announced payload size differ. Caller sends any
 * further payload words after this returns, then calls wait_for_token().
 * total_payload_bytes excludes the token. */
static unsigned long send_command_start(unsigned long command, unsigned long total_payload_bytes)
{
    unsigned long seed = rom3_read_long(RANDOM_TOKEN_SEED_OFFSET);

    (void)rom3_read(ROM3_PROTOCOL_HEADER);
    (void)rom3_read(command);
    (void)rom3_read(total_payload_bytes + 4UL);

    (void)rom3_read(seed & 0xFFFFUL);
    (void)rom3_read((seed >> 16) & 0xFFFFUL);

    return seed;
}

/* One 32-bit request parameter: low word, then high word (matches
 * GET_PAYLOAD_PARAM32). */
static void send_param32(unsigned long value)
{
    (void)rom3_read(value & 0xFFFFUL);
    (void)rom3_read((value >> 16) & 0xFFFFUL);
}

/* One 16-bit request parameter: a single raw-value read (matches
 * GET_PAYLOAD_PARAM16 = payload[0], no reassembly). */
static void send_param16(unsigned long value)
{
    (void)rom3_read(value & 0xFFFFUL);
}

/* A fixed-length string field, sent two raw source bytes at a time, packed
 * big-endian ((b0<<8)|b1), matching send_sync_write_command_to_sidecart's
 * word-copy loop. len must be even (all four string fields are). The
 * firmware's COPY_AND_CHANGE_ENDIANESS_BLOCK16 restores the original byte
 * order. */
static void send_string_field(const char *s, int len)
{
    int i;
    unsigned char b0, b1;

    for (i = 0; i < len; i += 2) {
        b0 = (unsigned char)s[i];
        b1 = (unsigned char)s[i + 1];
        (void)rom3_read(((unsigned long)b0 << 8) | (unsigned long)b1);
    }
}

/* A fixed-length string field, read byte-for-byte in address order (no
 * unswap needed on the Atari side -- see file header). Always forces the
 * destination's last byte to NUL, regardless of what the firmware sent. */
static void read_string_field(unsigned long offset, char *dest, int destsize)
{
    int i;
    for (i = 0; i < destsize; i++)
        dest[i] = (char)rom3_read(offset + (unsigned long)i);
    dest[destsize - 1] = '\0';
}

int floppy_probe_get_config_info(FloppyConfigInfo *info)
{
    unsigned long seed = send_command_start(CMD_FLOPPY_GET_CONFIG_INFO, 0UL);

    if (!wait_for_token(seed, PROBE_TIMEOUT_SEC))
        return FLOPPY_PROBE_TIMEOUT;

    info->protocol_version     = rom3_read_long(RESP_CONFIG_VERSION_OFFSET);
    info->max_profiles         = rom3_read_long(RESP_CONFIG_MAX_PROFILES_OFFSET);
    info->profile_count        = rom3_read_long(RESP_CONFIG_PROFILE_COUNT_OFFSET);
    info->active_profile_index = rom3_read_long(RESP_CONFIG_ACTIVE_INDEX_OFFSET);
    info->status                = rom3_read_long(RESP_CONFIG_STATUS_OFFSET);
    return FLOPPY_PROBE_OK;
}

int floppy_probe_get_profile(unsigned long index, FloppyProfileInfo *info)
{
    unsigned long seed = send_command_start(CMD_FLOPPY_GET_PROFILE, 4UL);
    send_param32(index);

    if (!wait_for_token(seed, PROBE_TIMEOUT_SEC))
        return FLOPPY_PROBE_TIMEOUT;

    info->status  = rom3_read_long(PROFILE_STATUS_OFFSET);
    info->state   = rom3_read_word(PROFILE_STATE_OFFSET);
    info->backend = rom3_read_word(PROFILE_BACKEND_OFFSET);
    info->port    = rom3_read_word(PROFILE_PORT_OFFSET);

    read_string_field(PROFILE_NICKNAME_OFFSET,      info->nickname,      FLOPPY_NICKNAME_LEN);
    read_string_field(PROFILE_LAST_DIRECTORY_OFFSET, info->last_directory, FLOPPY_LASTDIR_LEN);
    read_string_field(PROFILE_HOST_OFFSET,          info->host,          FLOPPY_HOST_LEN);
    read_string_field(PROFILE_MOUNT_PATH_OFFSET,    info->mount_path,    FLOPPY_MOUNTPATH_LEN);
    read_string_field(PROFILE_SD_PATH_OFFSET,       info->sd_path,       FLOPPY_SDPATH_LEN);
    return FLOPPY_PROBE_OK;
}

int floppy_probe_set_profile(unsigned long index, const FloppyProfileInfo *in, unsigned long *out_status)
{
    unsigned long seed = send_command_start(CMD_FLOPPY_SET_PROFILE, SET_PROFILE_PAYLOAD_BYTES);

    send_param32(index);
    send_param16(in->state);
    send_param16(in->backend);
    send_param16(in->port);
    send_string_field(in->nickname,      FLOPPY_NICKNAME_LEN);
    send_string_field(in->last_directory, FLOPPY_LASTDIR_LEN);
    send_string_field(in->host,          FLOPPY_HOST_LEN);
    send_string_field(in->mount_path,    FLOPPY_MOUNTPATH_LEN);
    send_string_field(in->sd_path,       FLOPPY_SDPATH_LEN);

    if (!wait_for_token(seed, PROBE_TIMEOUT_SEC))
        return FLOPPY_PROBE_TIMEOUT;

    *out_status = rom3_read_long(PROFILE_STATUS_OFFSET);
    return FLOPPY_PROBE_OK;
}

int floppy_probe_delete_profile(unsigned long index, unsigned long *out_status)
{
    unsigned long seed = send_command_start(CMD_FLOPPY_DELETE_PROFILE, 4UL);
    send_param32(index);

    if (!wait_for_token(seed, PROBE_TIMEOUT_SEC))
        return FLOPPY_PROBE_TIMEOUT;

    *out_status = rom3_read_long(PROFILE_STATUS_OFFSET);
    return FLOPPY_PROBE_OK;
}

int floppy_probe_set_active_profile(unsigned long index, unsigned long *out_status)
{
    unsigned long seed = send_command_start(CMD_FLOPPY_SET_ACTIVE_PROFILE, 4UL);
    send_param32(index);

    if (!wait_for_token(seed, PROBE_TIMEOUT_SEC))
        return FLOPPY_PROBE_TIMEOUT;

    *out_status = rom3_read_long(PROFILE_STATUS_OFFSET);
    return FLOPPY_PROBE_OK;
}

int floppy_probe_save_profiles(unsigned long *out_status)
{
    unsigned long seed = send_command_start(CMD_FLOPPY_SAVE_PROFILES, 0UL);

    if (!wait_for_token(seed, SAVE_PROFILES_TIMEOUT_SEC))
        return FLOPPY_PROBE_TIMEOUT;

    *out_status = rom3_read_long(PROFILE_STATUS_OFFSET);
    return FLOPPY_PROBE_OK;
}

/* ------------------------------------------------------------------
 * Step 2/3 -- real LFN directory browser (GEMDRVEMUL_FLOPPY_BROWSE_*,
 * subcommands 0x23-0x25). Offsets below are hand-walked from
 * gemdrvemul.h's own GEMDRVEMUL_FLOPPY_BROWSE/_PAGE macro chain
 * (floppyemu branch, SIDETNFS_NETWORK_ALIGN4-rounded, immediately after
 * GEMDRVEMUL_FLOPPY_PROFILE which ends at PROFILE_SD_PATH_OFFSET+256 =
 * 0x482E) -- not independently guessed. GET_PAGE's entries region is read
 * byte-for-byte in address order, same convention read_string_field()
 * already uses for every other Pico->Atari string field in this protocol
 * -- no unswap needed on this side (see this file's own top-of-file
 * byte-order summary).
 *
 * Step 3: the old separate GET_DIR_PAGE (0x25) / GET_FILE_PAGE (0x26)
 * were combined into one GET_PAGE, reusing 0x25's numeric value -- 0x26 is
 * retired/free again. The firmware itself now returns one page of up to
 * FLOPPY_BROWSE_PAGE_ENTRIES (15) entries, directories always sorted
 * before files within the page, so this side no longer fetches two pages
 * and stitches them together (that used to live in dialog.c's
 * fm_load_entries()). */
#define CMD_FLOPPY_BROWSE_OPEN        0x0423UL
#define CMD_FLOPPY_BROWSE_CHANGE_DIR  0x0424UL
#define CMD_FLOPPY_BROWSE_GET_PAGE    0x0425UL

#define BROWSE_STATUS_OFFSET     0x4830UL /* uint32_t */
#define BROWSE_GENERATION_OFFSET 0x4834UL /* uint32_t */
#define BROWSE_CWD_OFFSET        0x4838UL /* char[FLOPPY_BROWSE_CWD_LEN] -- block ends 0x4938 */

#define PAGE_STATUS_OFFSET     0x4938UL /* uint32_t */
#define PAGE_GENERATION_OFFSET 0x493CUL /* uint32_t */
#define PAGE_INDEX_OFFSET      0x4940UL /* uint32_t */
#define PAGE_COUNT_OFFSET      0x4944UL /* uint16_t */
#define PAGE_HAS_PREV_OFFSET   0x4946UL /* uint16_t */
#define PAGE_HAS_NEXT_OFFSET   0x4948UL /* uint16_t */
#define PAGE_ENTRIES_OFFSET    0x494CUL /* char[FLOPPY_BROWSE_PAGE_ENTRIES][FLOPPY_BROWSE_NAME_LEN] -- block ends 0x584C */
#define PAGE_IS_DIR_OFFSET     (PAGE_ENTRIES_OFFSET + (unsigned long)FLOPPY_BROWSE_PAGE_ENTRIES * (unsigned long)FLOPPY_BROWSE_NAME_LEN) /* uint16_t[FLOPPY_BROWSE_PAGE_ENTRIES], plain word per slot, 1=dir/0=file, index-matched with ENTRIES -- block ends 0x586A */

/* BROWSE_CHANGE_DIR request payload size, excluding the 4-byte token:
 * generation(4) + go_up(2) + name(FLOPPY_BROWSE_NAME_LEN=256) = 262 bytes. */
#define BROWSE_CHANGE_DIR_PAYLOAD_BYTES (4UL + 2UL + (unsigned long)FLOPPY_BROWSE_NAME_LEN)

static void read_browse_result(FloppyBrowseResult *out)
{
    out->status     = rom3_read_long(BROWSE_STATUS_OFFSET);
    out->generation = rom3_read_long(BROWSE_GENERATION_OFFSET);
    read_string_field(BROWSE_CWD_OFFSET, out->cwd, FLOPPY_BROWSE_CWD_LEN);
}

static void read_page_result(FloppyPageResult *out)
{
    unsigned int i;

    out->status     = rom3_read_long(PAGE_STATUS_OFFSET);
    out->generation = rom3_read_long(PAGE_GENERATION_OFFSET);
    out->page_index = rom3_read_long(PAGE_INDEX_OFFSET);
    out->count      = rom3_read_word(PAGE_COUNT_OFFSET);
    out->has_prev   = rom3_read_word(PAGE_HAS_PREV_OFFSET);
    out->has_next   = rom3_read_word(PAGE_HAS_NEXT_OFFSET);

    for (i = 0; i < FLOPPY_BROWSE_PAGE_ENTRIES; i++) {
        read_string_field(PAGE_ENTRIES_OFFSET + (unsigned long)i * FLOPPY_BROWSE_NAME_LEN,
                           out->entries[i], FLOPPY_BROWSE_NAME_LEN);
        out->is_dir[i] = rom3_read_word(PAGE_IS_DIR_OFFSET + (unsigned long)i * 2UL) ? 1 : 0;
    }
}

int floppy_probe_browse_open(unsigned long profile_index, FloppyBrowseResult *out)
{
    unsigned long seed = send_command_start(CMD_FLOPPY_BROWSE_OPEN, 4UL);
    send_param32(profile_index);

    if (!wait_for_token(seed, PROBE_TIMEOUT_SEC))
        return FLOPPY_PROBE_TIMEOUT;

    read_browse_result(out);
    return FLOPPY_PROBE_OK;
}

int floppy_probe_browse_change_dir(unsigned long generation, int go_up, const char *name, FloppyBrowseResult *out)
{
    char name_buf[FLOPPY_BROWSE_NAME_LEN];
    unsigned long seed;
    int i;

    /* send_string_field() reads exactly FLOPPY_BROWSE_NAME_LEN bytes from
     * its source -- pad with NULs past name's own terminator so nothing
     * past the caller's buffer is ever read. */
    for (i = 0; i < FLOPPY_BROWSE_NAME_LEN; i++) {
        name_buf[i] = (name != (const char *)0) ? name[i] : '\0';
        if (name_buf[i] == '\0')
            break;
    }
    for (; i < FLOPPY_BROWSE_NAME_LEN; i++)
        name_buf[i] = '\0';

    seed = send_command_start(CMD_FLOPPY_BROWSE_CHANGE_DIR, BROWSE_CHANGE_DIR_PAYLOAD_BYTES);
    send_param32(generation);
    send_param16(go_up ? 1UL : 0UL);
    send_string_field(name_buf, FLOPPY_BROWSE_NAME_LEN);

    if (!wait_for_token(seed, PROBE_TIMEOUT_SEC))
        return FLOPPY_PROBE_TIMEOUT;

    read_browse_result(out);
    return FLOPPY_PROBE_OK;
}

/* One GET_PAGE round trip. The firmware never blocks for long inside a
 * single call any more (SideTNFS-Firmware/romemul/include/
 * sidetnfs_floppy_browse.h: a TNFS walk only does a small, bounded number
 * of real network round trips per call), so PROBE_TIMEOUT_SEC is ample
 * headroom for THIS one call -- it is not the overall "is the page ready
 * yet" budget, see browse_get_page_poll() below for that. */
static int browse_get_page_once(unsigned long generation, unsigned long page_index, FloppyPageResult *out)
{
    unsigned long seed = send_command_start(CMD_FLOPPY_BROWSE_GET_PAGE, 8UL);
    send_param32(generation);
    send_param32(page_index);

    if (!wait_for_token(seed, PROBE_TIMEOUT_SEC))
        return FLOPPY_PROBE_TIMEOUT;

    read_page_result(out);
    return FLOPPY_PROBE_OK;
}

/* Overall budget across every resumed poll for ONE page fetch (not any
 * single round trip's own PROBE_TIMEOUT_SEC) -- generous enough for a
 * large directory walked a handful of TNFS entries at a time per poll. */
#define PAGE_POLL_TIMEOUT_SEC 30

/* A handful of quick, blind retries specifically for
 * FLOPPY_BROWSE_ERR_BACKEND_ERROR -- the firmware's catch-all for "the
 * TNFS/SD backend didn't cooperate", seen in practice roughly 1 in 10
 * listings and going away on its own on a later attempt. Confirmed with
 * the firmware side to be retry-safe: a BACKEND_ERROR only aborts that
 * one walk attempt (walk_active=false) and never touches the browse
 * session/generation itself, so a fresh identical GET_PAGE request is a
 * clean retry, no state to reconcile. It's also the expected statistical
 * tail of a *bounded* (not infinite) per-round TNFS retry inside the
 * firmware -- a directory walk needs many more network rounds than a
 * single file op, so the rare chance of one round exhausting its own
 * retries compounds over a whole page. No artificial delay between
 * attempts: a retry's own walk (via browse_get_page_poll()'s normal
 * IN_PROGRESS handling below) already takes however long it naturally
 * needs, so inserting a fixed pause first would just be an arbitrary
 * extra wait rather than anything calibrated to real page-fetch timing.
 * Capped low so a genuinely persistent backend problem still reaches the
 * user promptly instead of retrying indefinitely. */
#define BACKEND_ERROR_RETRY_COUNT 3

/* Re-issues the IDENTICAL GET_PAGE request (same generation/page_index)
 * for as long as the firmware reports FLOPPY_BROWSE_STATUS_IN_PROGRESS --
 * see sidetnfs_floppy_browse.h's own contract: the firmware never finishes
 * a deep/slow TNFS walk inside one blocking call, so the Atari side is the
 * one that keeps asking "is it ready yet" instead. Also silently retries a
 * few times on FLOPPY_BROWSE_ERR_BACKEND_ERROR (see that constant's own
 * comment above) before giving up and handing it back to the caller.
 * Transparent to floppy_probe_browse_get_page() -- out->status is never
 * FLOPPY_BROWSE_STATUS_IN_PROGRESS by the time this returns
 * FLOPPY_PROBE_OK, and is FLOPPY_BROWSE_ERR_BACKEND_ERROR only once every
 * retry has already been used up. */
static int browse_get_page_poll(unsigned long generation, unsigned long page_index, FloppyPageResult *out)
{
    long budget = (long)PAGE_POLL_TIMEOUT_SEC * PAL_VBLS_PER_SEC;
    int backend_retries_left = BACKEND_ERROR_RETRY_COUNT;
    int rc;

    for (;;) {
        rc = browse_get_page_once(generation, page_index, out);
        if (rc != FLOPPY_PROBE_OK)
            return rc;

        if (out->status == FLOPPY_BROWSE_STATUS_IN_PROGRESS) {
            if (budget <= 0)
                return FLOPPY_PROBE_TIMEOUT;
            Vsync();
            budget--;
            continue;
        }

        if (out->status == FLOPPY_BROWSE_ERR_BACKEND_ERROR && backend_retries_left > 0) {
            backend_retries_left--;
            continue;
        }

        return FLOPPY_PROBE_OK;
    }
}

int floppy_probe_browse_get_page(unsigned long generation, unsigned long page_index, FloppyPageResult *out)
{
    return browse_get_page_poll(generation, page_index, out);
}
