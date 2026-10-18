#include "client_decorations.h"

#include <X11/Xatom.h>

int macos_client_has_own_decorations(Display *display, Window window) {
    Atom frame_extents = XInternAtom(display, "_GTK_FRAME_EXTENTS", True);
    if (frame_extents != None) {
        Atom actual_type;
        int actual_format;
        unsigned long count;
        unsigned long bytes_after;
        unsigned char *data = NULL;
        int has_frame = XGetWindowProperty(
            display, window, frame_extents, 0, 4, False, XA_CARDINAL,
            &actual_type, &actual_format, &count, &bytes_after, &data) == Success &&
            data && actual_type == XA_CARDINAL && actual_format == 32 &&
            count == 4;
        if (data) XFree(data);
        if (has_frame) return 1;
    }

    Atom motif_hints = XInternAtom(display, "_MOTIF_WM_HINTS", True);
    if (motif_hints == None) return 0;

    Atom actual_type;
    int actual_format;
    unsigned long count;
    unsigned long bytes_after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(display, window, motif_hints, 0, 5, False,
                           motif_hints, &actual_type, &actual_format, &count,
                           &bytes_after, &data) != Success ||
        !data || actual_type != motif_hints || actual_format != 32 ||
        count < 3) {
        if (data) XFree(data);
        return 0;
    }

    const unsigned long *hints = (const unsigned long *)data;
    const unsigned long decorations_flag = 1UL << 1;
    int has_client_frame =
        (hints[0] & decorations_flag) && hints[2] == 0;
    XFree(data);
    return has_client_frame;
}
