#include "macoswm.h"
#include <stdio.h>

int main(int argc, char **argv) {
    const char *display_name = argc > 1 ? argv[1] : NULL;
    MacosWm *wm = macos_wm_new(display_name);
    if (!wm) return 1;

    int status = macos_wm_run(wm);
    macos_wm_free(wm);
    return status;
}
