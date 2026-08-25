/* FLOPTEST.PRG -- temporary Step 2 protocol exercise driver.
 *
 * NOT the final Step 3 browser UI -- this is a plain GEMDOS console
 * program (no AES/dialogs at all) that walks the real
 * GEMDRVEMUL_FLOPPY_BROWSE_* protocol end to end against whatever
 * profiles are already configured (via FLOPPY.PRG's own profile editor)
 * and prints what it gets back, so the protocol/firmware side of Step 2
 * can be verified without the final list-widget UI existing yet. Must be
 * deleted (and its Makefile rule removed) once Step 3 wires the real
 * browser into dialog.c's main window -- see this project's Step 2
 * report.
 *
 * Long names are always printed in full (Cconws() on a scrolling GEMDOS
 * console wraps them naturally if they exceed the console width) -- what
 * matters for the test is that the FULL name is what's stored in/compared
 * from FloppyPageResult.entries[], never a truncated one.
 */
#include <mint/osbind.h>
#include <stdio.h>
#include <string.h>
#include "floppy_probe.h"

static void out(const char *s)
{
    (void)Cconws(s);
}

static void outln(const char *s)
{
    (void)Cconws(s);
    (void)Cconws("\r\n");
}

static void wait_key(void)
{
    outln("");
    outln("-- press any key to continue --");
    Cconin();
    outln("");
}

static void print_page(const char *label, const FloppyPageResult *page)
{
    char buf[128];
    unsigned int i;

    sprintf(buf, "%s: status=%lu gen=%lu page=%lu count=%u prev=%u next=%u", label, page->status, page->generation,
            page->page_index, page->count, page->has_prev, page->has_next);
    outln(buf);
    for (i = 0; i < page->count; i++) {
        sprintf(buf, "  [%2u] ", i);
        out(buf);
        outln(page->entries[i]);
    }
}

/* Runs the full Step 2 protocol exercise against one already-configured
 * profile at `profile_index`: BROWSE_OPEN, GET_DIR_PAGE(0),
 * GET_FILE_PAGE(0), NEXT/PREVIOUS where a second page exists,
 * CHANGE_DIR into the first listed subdirectory (if any) followed by a
 * page fetch and a CHANGE_DIR back up. */
static void run_profile_test(unsigned long profile_index, const char *label)
{
    FloppyBrowseResult browse;
    FloppyPageResult dir_page, file_page;
    char buf[400];
    int rc;

    sprintf(buf, "=== %s (profile %lu) ===", label, profile_index);
    outln(buf);

    rc = floppy_probe_browse_open(profile_index, &browse);
    if (rc != FLOPPY_PROBE_OK) {
        outln("BROWSE_OPEN: TIMEOUT");
        wait_key();
        return;
    }
    sprintf(buf, "BROWSE_OPEN: status=%lu gen=%lu cwd=%s", browse.status, browse.generation, browse.cwd);
    outln(buf);
    if (browse.status != FLOPPY_BROWSE_OK) {
        wait_key();
        return;
    }

    rc = floppy_probe_browse_get_dir_page(browse.generation, 0UL, &dir_page);
    if (rc != FLOPPY_PROBE_OK) {
        outln("GET_DIR_PAGE(0): TIMEOUT");
        wait_key();
        return;
    }
    print_page("GET_DIR_PAGE(0)", &dir_page);
    wait_key();

    rc = floppy_probe_browse_get_file_page(browse.generation, 0UL, &file_page);
    if (rc != FLOPPY_PROBE_OK) {
        outln("GET_FILE_PAGE(0): TIMEOUT");
        wait_key();
        return;
    }
    print_page("GET_FILE_PAGE(0)", &file_page);
    wait_key();

    if (dir_page.has_next) {
        FloppyPageResult dir_page_next;
        rc = floppy_probe_browse_get_dir_page(browse.generation, 1UL, &dir_page_next);
        if (rc == FLOPPY_PROBE_OK) {
            print_page("GET_DIR_PAGE(1) [NEXT]", &dir_page_next);
            if (dir_page_next.has_prev) {
                FloppyPageResult dir_page_prev;
                rc = floppy_probe_browse_get_dir_page(browse.generation, 0UL, &dir_page_prev);
                if (rc == FLOPPY_PROBE_OK) {
                    print_page("GET_DIR_PAGE(0) [PREVIOUS]", &dir_page_prev);
                }
            }
        } else {
            outln("GET_DIR_PAGE(1): TIMEOUT");
        }
        wait_key();
    } else {
        outln("Only one directory page -- skipping NEXT/PREVIOUS test.");
        wait_key();
    }

    if (dir_page.count > 0) {
        FloppyBrowseResult sub;

        sprintf(buf, "CHANGE_DIR into '%s' ...", dir_page.entries[0]);
        outln(buf);
        rc = floppy_probe_browse_change_dir(browse.generation, 0, dir_page.entries[0], &sub);
        if (rc != FLOPPY_PROBE_OK) {
            outln("CHANGE_DIR: TIMEOUT");
            wait_key();
        } else {
            sprintf(buf, "CHANGE_DIR: status=%lu gen=%lu cwd=%s", sub.status, sub.generation, sub.cwd);
            outln(buf);
            if (sub.status == FLOPPY_BROWSE_OK) {
                FloppyPageResult sub_dir_page, sub_file_page;
                FloppyBrowseResult back;

                floppy_probe_browse_get_dir_page(sub.generation, 0UL, &sub_dir_page);
                print_page("  subdir GET_DIR_PAGE(0)", &sub_dir_page);
                floppy_probe_browse_get_file_page(sub.generation, 0UL, &sub_file_page);
                print_page("  subdir GET_FILE_PAGE(0)", &sub_file_page);

                rc = floppy_probe_browse_change_dir(sub.generation, 1, "", &back);
                if (rc == FLOPPY_PROBE_OK) {
                    sprintf(buf, "CHANGE_DIR('..'): status=%lu cwd=%s", back.status, back.cwd);
                    outln(buf);
                }
            }
            wait_key();
        }
    } else {
        outln("No subdirectory available -- skipping CHANGE_DIR test.");
        wait_key();
    }
}

static int find_profile(unsigned int want_backend, unsigned long max_profiles, unsigned long *out_index)
{
    unsigned long i;
    FloppyProfileInfo info;

    for (i = 0; i < max_profiles; i++) {
        if (floppy_probe_get_profile(i, &info) != FLOPPY_PROBE_OK)
            continue;
        if (info.status != FLOPPY_STATUS_OK)
            continue;
        if (info.state == FLOPPY_PROFILE_STATE_EMPTY)
            continue;
        if (info.backend != want_backend)
            continue;
        *out_index = i;
        return 1;
    }
    return 0;
}

int main(void)
{
    FloppyConfigInfo cfg;
    unsigned long tnfs_index = 0, sd_index = 0;
    char buf[128];

    outln("FLOPPY.PRG Step 2 protocol test driver");
    outln("(temporary -- not the final browser, see Step 3)");
    wait_key();

    if (floppy_probe_get_config_info(&cfg) != FLOPPY_PROBE_OK) {
        outln("GET_CONFIG_INFO: TIMEOUT -- is the cartridge present?");
        wait_key();
        return 1;
    }
    sprintf(buf, "GET_CONFIG_INFO: max=%lu count=%lu active=%lu status=%lu", cfg.max_profiles, cfg.profile_count,
            cfg.active_profile_index, cfg.status);
    outln(buf);
    wait_key();

    if (find_profile(FLOPPY_BACKEND_TNFS, cfg.max_profiles, &tnfs_index)) {
        run_profile_test(tnfs_index, "TNFS profile");
    } else {
        outln("No configured TNFS profile found -- skipping TNFS test.");
        wait_key();
    }

    if (find_profile(FLOPPY_BACKEND_SD, cfg.max_profiles, &sd_index)) {
        run_profile_test(sd_index, "SD profile");
    } else {
        outln("No configured SD profile found -- skipping SD test.");
        wait_key();
    }

    outln("Done.");
    wait_key();
    return 0;
}
