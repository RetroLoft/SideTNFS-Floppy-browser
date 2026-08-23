#include <gem.h>
#include <mint/osbind.h>
#include "dialog.h"
#include "profile.h"

int main(void)
{
    ProfileConfig cfg;

    appl_init();

    /* Low-resolution guard, copied from SIDETNFS-Config's main.c (see
     * RESEARCH-STEP0.md section 3.3) -- but deliberately WITHOUT its
     * Setscreen()+reset offer: FLOPPY.PRG must not change the Atari's
     * resolution itself, per the project brief. Medium/high only; low
     * resolution just explains why and exits. */
    if (Getrez() == 0) {
        form_alert(1, "[3][FLOPPY.PRG|This program requires|Medium or High resolution.][OK]");
        appl_exit();
        return 0;
    }

    graf_mouse(ARROW, (MFORM *)0);

    /* In-memory starting point only (no file I/O) -- dialog_run()
     * overrides this from the cartridge firmware when present, same
     * "no local config file" policy SIDETNFS.PRG documents. */
    profile_config_init_defaults(&cfg);
    dialog_run(&cfg);

    appl_exit();
    return 0;
}
