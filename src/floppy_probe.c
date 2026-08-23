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
 *   - uint16_t request params (SET_PROFILE's state/port): sent as ONE
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

/* GEMDRVEMUL_FLOPPY_PROFILE = CONFIG_STATUS + 4 = 0x45AC. */
#define PROFILE_STATUS_OFFSET   0x45ACUL /* uint32_t */
#define PROFILE_STATE_OFFSET    0x45B0UL /* uint16_t */
#define PROFILE_PORT_OFFSET     0x45B2UL /* uint16_t */
#define PROFILE_NICKNAME_OFFSET 0x45B4UL /* char[24] */
#define PROFILE_HOST_OFFSET     0x45CCUL /* char[64] */
#define PROFILE_MOUNT_PATH_OFFSET 0x460CUL /* char[32] */
#define PROFILE_LAST_DIRECTORY_OFFSET 0x462CUL /* char[256], block ends 0x472C */

/* SET_PROFILE request payload size, excluding the 4-byte token:
 * index(4) + state+port(2 each=4) + strings (24+64+32+256=376) = 384 bytes. */
#define SET_PROFILE_PAYLOAD_BYTES \
    (4UL + 2UL*2UL + (unsigned long)FLOPPY_NICKNAME_LEN + (unsigned long)FLOPPY_HOST_LEN + \
     (unsigned long)FLOPPY_MOUNTPATH_LEN + (unsigned long)FLOPPY_LASTDIR_LEN)

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

    info->status = rom3_read_long(PROFILE_STATUS_OFFSET);
    info->state  = rom3_read_word(PROFILE_STATE_OFFSET);
    info->port   = rom3_read_word(PROFILE_PORT_OFFSET);

    read_string_field(PROFILE_NICKNAME_OFFSET,      info->nickname,      FLOPPY_NICKNAME_LEN);
    read_string_field(PROFILE_HOST_OFFSET,           info->host,          FLOPPY_HOST_LEN);
    read_string_field(PROFILE_MOUNT_PATH_OFFSET,     info->mount_path,    FLOPPY_MOUNTPATH_LEN);
    read_string_field(PROFILE_LAST_DIRECTORY_OFFSET, info->last_directory, FLOPPY_LASTDIR_LEN);
    return FLOPPY_PROBE_OK;
}

int floppy_probe_set_profile(unsigned long index, const FloppyProfileInfo *in, unsigned long *out_status)
{
    unsigned long seed = send_command_start(CMD_FLOPPY_SET_PROFILE, SET_PROFILE_PAYLOAD_BYTES);

    send_param32(index);
    send_param16(in->state);
    send_param16(in->port);
    send_string_field(in->nickname,      FLOPPY_NICKNAME_LEN);
    send_string_field(in->host,          FLOPPY_HOST_LEN);
    send_string_field(in->mount_path,    FLOPPY_MOUNTPATH_LEN);
    send_string_field(in->last_directory, FLOPPY_LASTDIR_LEN);

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
