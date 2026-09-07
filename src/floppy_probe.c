/*
 * FLOPPY.PRG cartridge protocol: the LFN directory browser
 * (GEMDRVEMUL_FLOPPY_BROWSE_*) and the packed Favorites/Carousel upload +
 * session start (GEMDRVEMUL_FLOPPY_FAVORITES_* and SESSION_START).
 *
 * Architecture note: the old GET_CONFIG_INFO/GET_PROFILE/SET_PROFILE/
 * DELETE_PROFILE/SET_ACTIVE_PROFILE/SAVE_PROFILES commands (0x1D-0x22)
 * and their whole flash-backed profile store are GONE from the firmware
 * -- Browser Sources are local-only now (favcfg.c's CONFIG.CFG). Every
 * command below that used to take a profile_index now takes a
 * FloppySourceDescriptor (backend+port+host) directly instead.
 *
 * Protocol cross-checked against (read-only references, not modified):
 *   SideTNFS-Firmware/romemul/include/commands.h     (command codes, floppyemu/main branch)
 *   SideTNFS-Firmware/romemul/include/gemdrvemul.h   (offsets, walked from
 *     GEMDRVEMUL_FLOPPY_BROWSE, floppyemu/main branch)
 *   SideTNFS-Firmware/romemul/include/sidetnfs_floppy_emul.h (status enum
 *     values, field lengths, floppyemu/main branch)
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
#include <string.h>
#include "floppy_probe.h"

#define ROM3_BASE            0xFB0000UL
#define ROM3_PROTOCOL_HEADER 0xABCDUL /* CMD_MAGIC_NUMBER, gemdrive.s */

#define RANDOM_TOKEN_OFFSET      0x0000UL /* echoed token, polled after completion */
#define RANDOM_TOKEN_SEED_OFFSET 0x0004UL /* Pico-published seed, read before sending */

#define PROBE_TIMEOUT_SEC       2
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

/* ------------------------------------------------------------------
 * Step 2/3 -- real LFN directory browser (GEMDRVEMUL_FLOPPY_BROWSE_*,
 * subcommands 0x23-0x25).
 *
 * Offsets below shifted down once the old GEMDRVEMUL_FLOPPY_PROFILE
 * block (which used to sit just before this one, ending at 0x482E) was
 * removed from the firmware along with the whole flash profile store
 * (see this file's own architecture-change header comment) -- current
 * values confirmed directly against the firmware's own compiled
 * preprocessor output by the firmware side, not hand-walked/guessed.
 *
 * GET_PAGE's entries region is read byte-for-byte in address order, same
 * convention read_string_field() already uses for every other
 * Pico->Atari string field in this protocol -- no unswap needed on this
 * side (see this file's own top-of-file byte-order summary). */
#define CMD_FLOPPY_BROWSE_OPEN        0x0423UL
#define CMD_FLOPPY_BROWSE_CHANGE_DIR  0x0424UL
#define CMD_FLOPPY_BROWSE_GET_PAGE    0x0425UL

#define BROWSE_STATUS_OFFSET     0x4598UL /* uint32_t */
#define BROWSE_GENERATION_OFFSET 0x459CUL /* uint32_t */
#define BROWSE_CWD_OFFSET        0x45A0UL /* char[FLOPPY_BROWSE_CWD_LEN] */

#define PAGE_STATUS_OFFSET     0x46A0UL /* uint32_t */
#define PAGE_GENERATION_OFFSET 0x46A4UL /* uint32_t */
#define PAGE_INDEX_OFFSET      0x46A8UL /* uint32_t */
#define PAGE_COUNT_OFFSET      0x46ACUL /* uint16_t */
#define PAGE_HAS_PREV_OFFSET   0x46AEUL /* uint16_t */
#define PAGE_HAS_NEXT_OFFSET   0x46B0UL /* uint16_t */
#define PAGE_ENTRIES_OFFSET    0x46B4UL /* char[FLOPPY_BROWSE_PAGE_ENTRIES][FLOPPY_BROWSE_NAME_LEN] */
#define PAGE_IS_DIR_OFFSET     (PAGE_ENTRIES_OFFSET + (unsigned long)FLOPPY_BROWSE_PAGE_ENTRIES * (unsigned long)FLOPPY_BROWSE_NAME_LEN) /* uint16_t[FLOPPY_BROWSE_PAGE_ENTRIES] -- computed, confirmed to land on 0x55B4 */

/* BROWSE_OPEN request payload size, excluding the 4-byte token:
 * backend(2) + port(2) + host(FLOPPY_HOST_LEN=64) +
 * start_directory(FLOPPY_STARTDIR_LEN=256) = 324 bytes. Built with this
 * file's own uniform send_param16()/send_string_field() mechanism, same
 * as every other command here (including bulk string-heavy ones like the
 * old SET_PROFILE or FAVORITES_WRITE_CHUNK below) -- there is no separate
 * "bulk transfer"/header-skip transport in this file at all, so none is
 * added here either; flagged explicitly to the firmware side in case
 * their own BROWSE_OPEN parser expects one. */
#define BROWSE_OPEN_PAYLOAD_BYTES (2UL + 2UL + (unsigned long)FLOPPY_HOST_LEN + (unsigned long)FLOPPY_STARTDIR_LEN)

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

/* Copies src (NUL-terminated, truncated if longer than len) into buf,
 * zero-padding the remainder -- send_string_field() always reads exactly
 * len bytes, so this guarantees nothing past src's own NUL (or past the
 * caller's buffer, if src has no NUL within len bytes) is ever read.
 * src may be NULL (treated as ""). Same padding idea
 * floppy_probe_browse_change_dir()/floppy_probe_session_start() already
 * used inline for their own single string field each -- factored out
 * here since BROWSE_OPEN/SESSION_START now each send two. */
static void pad_field(char *buf, int len, const char *src)
{
    int i;
    for (i = 0; i < len; i++) {
        buf[i] = (src != (const char *)0) ? src[i] : '\0';
        if (buf[i] == '\0')
            break;
    }
    for (; i < len; i++)
        buf[i] = '\0';
}

int floppy_probe_browse_open(const FloppySourceDescriptor *src, const char *start_directory, FloppyBrowseResult *out)
{
    char host_buf[FLOPPY_HOST_LEN];
    char start_dir_buf[FLOPPY_STARTDIR_LEN];
    unsigned long seed;

    pad_field(host_buf, FLOPPY_HOST_LEN, src->host);
    pad_field(start_dir_buf, FLOPPY_STARTDIR_LEN, start_directory);

    seed = send_command_start(CMD_FLOPPY_BROWSE_OPEN, BROWSE_OPEN_PAYLOAD_BYTES);
    send_param16(src->backend);
    send_param16(src->port);
    send_string_field(host_buf, FLOPPY_HOST_LEN);
    send_string_field(start_dir_buf, FLOPPY_STARTDIR_LEN);

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

/* ------------------------------------------------------------------
 * Phase 5 -- packed Favorites upload + session start
 * (GEMDRVEMUL_FLOPPY_FAVORITES_WRITE_CHUNK/_WRITE_CHECK/_COMMIT/
 * GEMDRVEMUL_FLOPPY_SESSION_START, subcommands 0x26-0x29).
 *
 * Response offsets below shifted along with the browser block above
 * (same removed-profile-block cause), confirmed directly against the
 * firmware's own compiled preprocessor output by the firmware side. Only
 * offsets actually dereferenced via rom3_read*() below are kept as
 * constants; the REQUEST-side fields (table, strings, count, active_index,
 * strings_used, chunk offset/length, session backend/port/host/
 * install_gemdrive/install_floppy/image_path) are never read back at a
 * fixed address at all -- every one of them is sent as a plain
 * sequential value/string stream via send_param16()/send_param32()/
 * send_string_field()/send_bulk_field_checksummed(), the same uniform
 * mechanism this file uses for every command, so they need no ROM3
 * address constant on this side regardless of any upstream shift. The
 * per-entry favorites/carousel table also WIDENED as part of the
 * architecture change that removed the old flash profile store: 60
 * plain uint16_t path-offset words became 60 x FavcfgTableEntry (4
 * words each: backend/port/host_offset/path_offset) -- see favcfg.h. */
#define CMD_FLOPPY_FAVORITES_WRITE_CHUNK 0x0426UL
#define CMD_FLOPPY_FAVORITES_WRITE_CHECK 0x0427UL
#define CMD_FLOPPY_FAVORITES_COMMIT      0x0428UL
#define CMD_FLOPPY_SESSION_START         0x0429UL

#define FAV_STATUS_OFFSET          0x55ECUL /* uint32_t -- response */
#define FAV_CHUNK_CHECKSUM_OFFSET  0x55EAUL /* uint16_t -- response */

#define SESSION_STATUS_OFFSET             0x97D0UL /* uint32_t -- response */
#define SESSION_SIDES_OFFSET              0x99E0UL /* uint16_t -- response */
#define SESSION_SECTORS_PER_TRACK_OFFSET  0x99E2UL /* uint16_t -- response */
#define SESSION_TRACKS_OFFSET             0x99E4UL /* uint16_t -- response */
#define SESSION_BYTES_PER_SECTOR_OFFSET   0x99E6UL /* uint16_t -- response */

/* SESSION_START request payload: backend(2) + port(2) + host
 * (FLOPPY_HOST_LEN=64) + install_gemdrive(2) + install_floppy(2) +
 * drive_number(2) + image_path(FLOPPY_SESSION_IMAGE_PATH_MAX=512) = 586
 * bytes, comfortably under the 2112-byte payload cap. drive_number (0 =
 * drive A:, 1 = drive B:) is the newest field, inserted right after
 * install_floppy and before image_path -- matches SideTNFS-Firmware's own
 * updated GEMDRVEMUL_FLOPPY_SESSION_START layout exactly (drive A:/B:
 * selection, coordinated with that repo). This is the FIRST Carousel
 * entry's own descriptor sent inline -- there is no more session-wide
 * "active_slot" on the firmware side at all (GEMDRVEMUL_FLOPPY_
 * SESSION_ACTIVE_SLOT is removed from the ROM3 layout entirely per the
 * architecture change, not just unused). No leading header/skip words --
 * same uniform payload convention BROWSE_OPEN above uses, confirmed
 * against this file's own existing (pre-change) SESSION_START, which
 * never had one either. */
#define SESSION_START_PAYLOAD_BYTES \
    (2UL + 2UL + (unsigned long)FLOPPY_HOST_LEN + 2UL + 2UL + 2UL + (unsigned long)FLOPPY_SESSION_IMAGE_PATH_MAX)

/* Sends len (must be even) bytes from data as a raw byte blob -- same
 * wire shape send_string_field() already uses (two source bytes packed
 * big-endian per address-encoded read), just parameterized on length
 * instead of a fixed field size, since a Favorites chunk's length varies
 * per call. Returns a running 16-bit word-sum checksum of the bytes just
 * sent, computed the same simple wraparound-add way the firmware's own
 * WRITE_CHUNK handler computes it from what it received -- letting the
 * caller compare its own view against the firmware's without a second
 * round trip. */
static unsigned short send_bulk_field_checksummed(const char *data, int len)
{
    int i;
    unsigned char b0, b1;
    unsigned short checksum = 0;

    for (i = 0; i < len; i += 2) {
        b0 = (unsigned char)data[i];
        b1 = (unsigned char)data[i + 1];
        (void)rom3_read(((unsigned long)b0 << 8) | (unsigned long)b1);
        checksum = (unsigned short)(checksum + (unsigned short)(((unsigned short)b0 << 8) | b1));
    }
    return checksum;
}

/* One WRITE_CHUNK + WRITE_CHECK pair for exactly one chunk. Returns
 * FLOPPY_PROBE_OK/_TIMEOUT for the transport; *out_status is the
 * firmware's FLOPPY_SESSION_* result (checked against both WRITE_CHUNK's
 * own checksum-compare, done locally here, and WRITE_CHECK's own
 * independent re-verification of what's actually sitting in ROM3). */
static int favorites_write_one_chunk(unsigned long offset, const char *data, unsigned int length,
                                       unsigned long *out_status)
{
    unsigned long seed;
    unsigned short local_checksum, firmware_checksum;
    unsigned long payload_bytes;

    payload_bytes = 4UL + 2UL + (unsigned long)length;
    seed = send_command_start(CMD_FLOPPY_FAVORITES_WRITE_CHUNK, payload_bytes);
    send_param32(offset);
    send_param16((unsigned long)length);
    local_checksum = send_bulk_field_checksummed(data, (int)length);

    if (!wait_for_token(seed, PROBE_TIMEOUT_SEC))
        return FLOPPY_PROBE_TIMEOUT;

    *out_status = rom3_read_long(FAV_STATUS_OFFSET);
    if (*out_status != FLOPPY_SESSION_OK)
        return FLOPPY_PROBE_OK;

    firmware_checksum = rom3_read_word(FAV_CHUNK_CHECKSUM_OFFSET);
    if (firmware_checksum != local_checksum) {
        *out_status = FLOPPY_SESSION_ERR_BACKEND_ERROR;
        return FLOPPY_PROBE_OK;
    }

    /* WRITE_CHECK: zero payload -- re-verifies the just-written ROM3
     * range against the checksum WRITE_CHUNK already reported (see its
     * own comment in commands.h/gemdrvemul.c: a genuine post-write
     * integrity check, not a rubber stamp). */
    seed = send_command_start(CMD_FLOPPY_FAVORITES_WRITE_CHECK, 0UL);
    if (!wait_for_token(seed, PROBE_TIMEOUT_SEC))
        return FLOPPY_PROBE_TIMEOUT;

    *out_status = rom3_read_long(FAV_STATUS_OFFSET);
    return FLOPPY_PROBE_OK;
}

int floppy_probe_favorites_upload(const FavcfgSession *session, unsigned long *out_status)
{
    unsigned long offset;
    unsigned long seed;
    int i, rc;

    for (offset = 0; offset < session->strings_used; offset += FLOPPY_FAVORITES_CHUNK_MAX) {
        unsigned long remaining = session->strings_used - offset;
        unsigned int length = (remaining > FLOPPY_FAVORITES_CHUNK_MAX) ? FLOPPY_FAVORITES_CHUNK_MAX
                                                                         : (unsigned int)remaining;
        /* WRITE_CHUNK/WRITE_CHECK require an even length (see
         * CHANGE_ENDIANESS_BLOCK16 on the firmware side) -- pad the final chunk by one harmless NUL
         * byte if strings_used happens to be odd. Safe: this byte lands
         * past the offset table ever points to (strings_used already
         * accounts for every real string's own NUL), so nothing ever
         * reads it back as meaningful data. */
        char chunk_buf[FLOPPY_FAVORITES_CHUNK_MAX];
        unsigned int copy_len = length;
        if ((length % 2) != 0) {
            memcpy(chunk_buf, session->strings + offset, (size_t)length);
            chunk_buf[length] = '\0';
            copy_len = length + 1;
        }

        rc = favorites_write_one_chunk(offset, (copy_len == length) ? (session->strings + offset) : chunk_buf,
                                         copy_len, out_status);
        if (rc != FLOPPY_PROBE_OK)
            return rc;
        if (*out_status != FLOPPY_SESSION_OK)
            return FLOPPY_PROBE_OK;
    }

    /* COMMIT: table[60] entries x 4 sequential plain uint16_t words each
     * (backend, port, host_offset, path_offset -- NOT a byte blob,
     * send_param16() per word, matching GET_PAYLOAD_PARAM16 on the
     * firmware side, same as the old path-offset-only table did for its
     * one word per entry) + count(2) + active_index(2) +
     * strings_used(4). Table WIDENED from 60 words to 60x4 words per the
     * architecture change -- see favcfg.h's own FavcfgTableEntry
     * comment. */
    seed = send_command_start(CMD_FLOPPY_FAVORITES_COMMIT,
                                (unsigned long)FAVCFG_SESSION_MAX_COUNT * 4UL * 2UL + 2UL + 2UL + 4UL);
    for (i = 0; i < FAVCFG_SESSION_MAX_COUNT; i++) {
        send_param16((unsigned long)session->table[i].backend);
        send_param16((unsigned long)session->table[i].port);
        send_param16((unsigned long)session->table[i].host_offset);
        send_param16((unsigned long)session->table[i].path_offset);
    }
    send_param16((unsigned long)session->count);
    send_param16((unsigned long)session->active_index);
    send_param32(session->strings_used);

    if (!wait_for_token(seed, PROBE_TIMEOUT_SEC))
        return FLOPPY_PROBE_TIMEOUT;

    *out_status = rom3_read_long(FAV_STATUS_OFFSET);
    return FLOPPY_PROBE_OK;
}

int floppy_probe_session_start(const FloppySourceDescriptor *src, const char *image_path,
                                 int install_gemdrive, int install_floppy, int drive_number,
                                 FloppySessionResult *out)
{
    char host_buf[FLOPPY_HOST_LEN];
    char path_buf[FLOPPY_SESSION_IMAGE_PATH_MAX];
    unsigned long seed;

    pad_field(host_buf, FLOPPY_HOST_LEN, src->host);
    pad_field(path_buf, FLOPPY_SESSION_IMAGE_PATH_MAX, image_path);

    seed = send_command_start(CMD_FLOPPY_SESSION_START, SESSION_START_PAYLOAD_BYTES);
    send_param16(src->backend);
    send_param16(src->port);
    send_string_field(host_buf, FLOPPY_HOST_LEN);
    send_param16(install_gemdrive ? 1UL : 0UL);
    send_param16(install_floppy ? 1UL : 0UL);
    send_param16(drive_number ? 1UL : 0UL); /* 0 = drive A: (default), 1 = drive B: */
    send_string_field(path_buf, FLOPPY_SESSION_IMAGE_PATH_MAX);

    /* SESSION_START may need real network/backend I/O (TNFS mount, sector
     * 0 read+validate) -- give it more room than the small config-style
     * calls above, same reasoning PAGE_POLL_TIMEOUT_SEC/
     * SAVE_PROFILES_TIMEOUT_SEC already use elsewhere in this file. */
    if (!wait_for_token(seed, PAGE_POLL_TIMEOUT_SEC))
        return FLOPPY_PROBE_TIMEOUT;

    out->status             = rom3_read_long(SESSION_STATUS_OFFSET);
    out->sides              = rom3_read_word(SESSION_SIDES_OFFSET);
    out->sectors_per_track  = rom3_read_word(SESSION_SECTORS_PER_TRACK_OFFSET);
    out->tracks             = rom3_read_word(SESSION_TRACKS_OFFSET);
    out->bytes_per_sector   = rom3_read_word(SESSION_BYTES_PER_SECTOR_OFFSET);
    return FLOPPY_PROBE_OK;
}
