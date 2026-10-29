/* Test helper: injects key events into the Terminator window and saves screenshots (PPM).
 * stdin script:  T text | K keysym [ctrl] [shift] | S ms | SHOT file.ppm | CLICK x y            */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static Display *d; static Window w;
static void key(KeySym ks, unsigned st)
{
    XKeyEvent e; memset(&e, 0, sizeof e);
    e.type = KeyPress; e.display = d; e.window = w; e.root = DefaultRootWindow(d); e.subwindow = None;
    e.time = CurrentTime; e.same_screen = True; e.state = st; e.keycode = XKeysymToKeycode(d, ks);
    XSendEvent(d, w, True, KeyPressMask, (XEvent *)&e); XFlush(d); usleep(30000);
}
static Window find(Window root)
{
    Window r, p, *kids; unsigned n; Window found = 0;
    if (!XQueryTree(d, root, &r, &p, &kids, &n)) return 0;
    for (unsigned i = 0; i < n && !found; i++) {
        char *nm = NULL;
        if (XFetchName(d, kids[i], &nm) && nm) { if (!strcmp(nm, "Terminator - Custom Terminal")) found = kids[i]; XFree(nm); }
        if (!found) found = find(kids[i]);
    }
    XFree(kids); return found;
}
int main(void)
{
    d = XOpenDisplay(NULL); if (!d) return 1;
    for (int i = 0; i < 100 && !(w = find(DefaultRootWindow(d))); i++) usleep(100000);
    if (!w) { fprintf(stderr, "no window\n"); return 2; }
    char line[1024];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\n")] = 0;
        if (line[0] == 'T') for (char *c = line + 2; *c; c++) {
            KeySym ks = *c == ' ' ? XK_space : (KeySym)(unsigned char)*c;
            unsigned st = 0;
            KeyCode kc = XKeysymToKeycode(d, ks);
            if (XKeycodeToKeysym(d, kc, 0) != ks) st = ShiftMask;
            key(ks, st);
        } else if (line[0] == 'K') {
            char name[64]; unsigned st = 0; sscanf(line + 2, "%63s", name);
            if (strstr(line, " ctrl")) st |= ControlMask;
            if (strstr(line, " shift")) st |= ShiftMask;
            key(XStringToKeysym(name), st);
        } else if (!strncmp(line, "CLICK", 5)) {
            int x, y; sscanf(line + 6, "%d %d", &x, &y);
            XButtonEvent b; memset(&b, 0, sizeof b);
            b.type = ButtonPress; b.display = d; b.window = w; b.root = DefaultRootWindow(d); b.x = x; b.y = y; b.button = Button1; b.same_screen = True;
            XSendEvent(d, w, True, ButtonPressMask, (XEvent *)&b);
            b.type = ButtonRelease; XSendEvent(d, w, True, ButtonReleaseMask, (XEvent *)&b); XFlush(d); usleep(50000);
        } else if (!strncmp(line, "SHOT ", 5)) {
            XWindowAttributes a; XGetWindowAttributes(d, w, &a);
            XImage *im = XGetImage(d, w, 0, 0, a.width, a.height, AllPlanes, ZPixmap);
            FILE *f = fopen(line + 5, "wb"); fprintf(f, "P6\n%d %d\n255\n", a.width, a.height);
            for (int y = 0; y < a.height; y++) for (int x = 0; x < a.width; x++) {
                unsigned long p = XGetPixel(im, x, y);
                fputc((p >> 16) & 255, f); fputc((p >> 8) & 255, f); fputc(p & 255, f);
            }
            fclose(f); XDestroyImage(im);
        } else if (line[0] == 'S') { usleep(atoi(line + 2) * 1000); }
    }
    return 0;
}
