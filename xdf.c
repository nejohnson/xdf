#define _POSIX_C_SOURCE 200809L

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <time.h>

typedef struct {
    const char *path;
    const char *display_name;
    const char *geometry;
    const char *label;
    const char *foreground;
    const char *background;
    const char *highlight;
    int update;
    bool show_label;
} Options;

typedef struct {
    Display *display;
    Window window;
    GC foreground;
    GC background;
    GC highlight;
    XFontStruct *font;
    unsigned int width;
    unsigned int height;
    double *history;
    size_t count;
    size_t capacity;
    double current;
    const Options *options;
} Graph;

static void usage(FILE *out)
{
    fprintf(out, "Usage: xdf [-path directory] [-update seconds] [-label text | -nolabel]\n"
                 "           [-display display] [-geometry geometry] [-fg color] [-bg color]\n"
                 "           [-hl color]\n"
                 "Plot filesystem usage as a percentage, updating every 10 seconds.\n"
                 "The default path is /.\n");
}

static const char *option_value(int argc, char **argv, int *i)
{
    if (*i + 1 >= argc) {
        fprintf(stderr, "xdf: missing value for %s\n", argv[*i]);
        exit(EXIT_FAILURE);
    }
    return argv[++*i];
}

static void parse_options(int argc, char **argv, Options *options)
{
    *options = (Options){ .path = "/", .update = 10, .show_label = true,
                          .foreground = "black", .background = "white",
                          .highlight = "gray60" };
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (!strcmp(arg, "-help") || !strcmp(arg, "--help")) {
            usage(stdout);
            exit(EXIT_SUCCESS);
        } else if (!strcmp(arg, "-path")) {
            options->path = option_value(argc, argv, &i);
        } else if (!strcmp(arg, "-update")) {
            const char *value = option_value(argc, argv, &i);
            char *end;
            errno = 0;
            long seconds = strtol(value, &end, 10);
            if (errno || *end || end == value || seconds < 1 || seconds > 86400) {
                fprintf(stderr, "xdf: update must be 1-86400 seconds\n");
                exit(EXIT_FAILURE);
            }
            options->update = (int)seconds;
        } else if (!strcmp(arg, "-label")) {
            options->label = option_value(argc, argv, &i);
            options->show_label = true;
        } else if (!strcmp(arg, "-nolabel")) {
            options->show_label = false;
        } else if (!strcmp(arg, "-display")) {
            options->display_name = option_value(argc, argv, &i);
        } else if (!strcmp(arg, "-geometry")) {
            options->geometry = option_value(argc, argv, &i);
        } else if (!strcmp(arg, "-fg") || !strcmp(arg, "-foreground")) {
            options->foreground = option_value(argc, argv, &i);
        } else if (!strcmp(arg, "-bg") || !strcmp(arg, "-background")) {
            options->background = option_value(argc, argv, &i);
        } else if (!strcmp(arg, "-hl") || !strcmp(arg, "-highlight")) {
            options->highlight = option_value(argc, argv, &i);
        } else {
            fprintf(stderr, "xdf: unknown option: %s\n", arg);
            usage(stderr);
            exit(EXIT_FAILURE);
        }
    }
}

/* Match df's Use% denominator: blocks already used plus blocks available to
   an ordinary user. Reserved blocks therefore count toward usage. */
static int disk_percentage(const char *path, double *percentage)
{
    struct statvfs fs;
    if (statvfs(path, &fs) < 0)
        return -1;
    double used = (double)fs.f_blocks - (double)fs.f_bfree;
    double available = (double)fs.f_bavail;
    double denominator = used + available;
    if (denominator <= 0.0) {
        errno = ERANGE;
        return -1;
    }
    *percentage = 100.0 * used / denominator;
    if (*percentage < 0.0) *percentage = 0.0;
    if (*percentage > 100.0) *percentage = 100.0;
    return 0;
}

/* Allocate a named colour, also storing it as opaque ARGB in argb if set. */
static unsigned long color_pixel(Display *display, const char *name, unsigned long *argb)
{
    XColor color, exact;
    if (!XAllocNamedColor(display, DefaultColormap(display, DefaultScreen(display)),
                          name, &color, &exact)) {
        fprintf(stderr, "xdf: invalid color: %s\n", name);
        exit(EXIT_FAILURE);
    }
    if (argb)
        *argb = 0xff000000UL | (unsigned long)(color.red >> 8) << 16 |
                (unsigned long)(color.green >> 8) << 8 | (unsigned long)(color.blue >> 8);
    return color.pixel;
}

static GC make_gc(Display *display, Window window, unsigned long pixel)
{
    XGCValues values = { .foreground = pixel };
    return XCreateGC(display, window, GCForeground, &values);
}

static void resize_history(Graph *graph, unsigned int width)
{
    size_t capacity = width;
    if (capacity == graph->capacity) return;
    size_t keep = graph->count < capacity ? graph->count : capacity;
    if (keep)
        memmove(graph->history, graph->history + graph->count - keep,
                keep * sizeof(*graph->history));
    double *history = realloc(graph->history, capacity * sizeof(*history));
    if (!history && capacity) {
        fprintf(stderr, "xdf: out of memory\n");
        exit(EXIT_FAILURE);
    }
    graph->history = history;
    graph->capacity = capacity;
    graph->count = keep;
}

static void append_sample(Graph *graph, double percentage)
{
    graph->current = percentage;
    if (!graph->capacity) return;
    if (graph->count == graph->capacity) {
        memmove(graph->history, graph->history + 1,
                (graph->count - 1) * sizeof(*graph->history));
        --graph->count;
    }
    graph->history[graph->count++] = percentage;
}

static void draw(Graph *graph)
{
    Display *display = graph->display;
    Window window = graph->window;
    unsigned int width = graph->width, height = graph->height;
    if (!width || !height) return;
    XFillRectangle(display, window, graph->background, 0, 0, width, height);

    int top = 0;
    if (graph->options->show_label) {
        int baseline = graph->font->ascent + 3;
        char label[1024];
        const char *name = graph->options->label ? graph->options->label
                                                  : graph->options->path;
        int shown = (int)graph->current;
        if ((double)shown < graph->current) ++shown;
        snprintf(label, sizeof(label), "%s: %d%%", name, shown);
        XDrawString(display, window, graph->foreground, 3, baseline,
                    label, (int)strlen(label));
        top = graph->font->ascent + graph->font->descent + 6;
    }
    int bottom = (int)height - 1;
    int graph_height = bottom - top;
    if (graph_height <= 0) return;

    for (size_t i = 0; i < graph->count; ++i) {
        int x = (int)(width - graph->count + i);
        int bar_height = (int)(graph->history[i] * graph_height / 100.0 + 0.5);
        if (bar_height > 0)
            XDrawLine(display, window, graph->foreground, x,
                      bottom - bar_height + 1, x, bottom);
    }
    for (int mark = 25; mark <= 75; mark += 25) {
        int y = bottom - graph_height * mark / 100;
        XDrawLine(display, window, graph->highlight, 0, y, (int)width - 1, y);
    }
}

/* Whether (u, v) lies inside a square inset from the 0-1 unit square by
   inset on each side, with corners of the given radius. */
static bool in_rounded_square(double u, double v, double inset, double radius)
{
    double lo = inset + radius, hi = 1.0 - inset - radius;
    double dx = u < lo ? lo - u : u > hi ? u - hi : 0.0;
    double dy = v < lo ? lo - v : v > hi ? v - hi : 0.0;
    if (u < inset || u > 1.0 - inset || v < inset || v > 1.0 - inset) return false;
    return dx * dx + dy * dy <= radius * radius;
}

/* Icon colours as opaque ARGB, from the graph's colour options. */
typedef struct {
    unsigned long foreground;
    unsigned long background;
    unsigned long highlight;
} IconColors;

/* Colour of the icon at (u, v), both 0-1 with v = 0 at the top: a miniature
   of the graph in a rounded frame. Returns ARGB, transparent outside it. */
static unsigned long icon_color(double u, double v, const IconColors *colors)
{
    const double margin = 0.04, radius = 0.18, frame = 0.07;
    if (!in_rounded_square(u, v, margin, radius)) return 0;
    double inner = margin + frame;
    if (!in_rounded_square(u, v, inner, radius - frame)) return colors->foreground;
    double h = (1.0 - inner - v) / (1.0 - 2.0 * inner);
    for (int mark = 1; mark <= 3; ++mark)
        if (h > mark * 0.25 - 0.025 && h < mark * 0.25 + 0.025) return colors->highlight;
    double used = 0.30 + 0.35 * u + 0.06 * sin(u * 9.0);
    return h < used ? colors->foreground : colors->background;
}

/* Set _NET_WM_ICON so panels and task switchers can show something better
   than a generic X. Each size is supersampled 4x4 for smooth edges. */
static void set_icon(Display *display, Window window, const IconColors *colors)
{
    static const int sizes[] = { 16, 24, 32, 48, 64 };
    enum { SUB = 4 };
    size_t total = 0;
    for (size_t i = 0; i < sizeof(sizes) / sizeof(*sizes); ++i)
        total += 2 + (size_t)sizes[i] * (size_t)sizes[i];
    /* Format 32 properties are passed as longs whatever their size. */
    unsigned long *data = malloc(total * sizeof(*data));
    if (!data) return;
    unsigned long *p = data;
    for (size_t i = 0; i < sizeof(sizes) / sizeof(*sizes); ++i) {
        int n = sizes[i];
        *p++ = (unsigned long)n;
        *p++ = (unsigned long)n;
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) {
                unsigned int a = 0, r = 0, g = 0, b = 0;
                for (int sy = 0; sy < SUB; ++sy) {
                    for (int sx = 0; sx < SUB; ++sx) {
                        unsigned long c = icon_color((x + (sx + 0.5) / SUB) / n,
                                                     (y + (sy + 0.5) / SUB) / n, colors);
                        unsigned int ca = (c >> 24) & 0xff;
                        a += ca;
                        r += ((c >> 16) & 0xff) * ca / 255;
                        g += ((c >> 8) & 0xff) * ca / 255;
                        b += (c & 0xff) * ca / 255;
                    }
                }
                /* Average premultiplied samples, then un-premultiply. */
                a /= SUB * SUB;
                if (a) {
                    r = r / (SUB * SUB) * 255 / a;
                    g = g / (SUB * SUB) * 255 / a;
                    b = b / (SUB * SUB) * 255 / a;
                }
                *p++ = (unsigned long)a << 24 | (unsigned long)(r > 255 ? 255 : r) << 16 |
                       (unsigned long)(g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
            }
        }
    }
    Atom net_wm_icon = XInternAtom(display, "_NET_WM_ICON", False);
    XChangeProperty(display, window, net_wm_icon, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)data, (int)total);
    free(data);
}

static long long monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        perror("xdf: clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
    Options options;
    parse_options(argc, argv, &options);
    double percentage;
    if (disk_percentage(options.path, &percentage) < 0) {
        fprintf(stderr, "xdf: %s: %s\n", options.path, strerror(errno));
        return EXIT_FAILURE;
    }

    Display *display = XOpenDisplay(options.display_name);
    if (!display) {
        fprintf(stderr, "xdf: cannot open display %s\n",
                options.display_name ? options.display_name :
                (getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)"));
        return EXIT_FAILURE;
    }
    int screen = DefaultScreen(display);
    int x = 0, y = 0;
    unsigned int width = 160, height = 80;
    if (options.geometry) {
        int flags = XParseGeometry(options.geometry, &x, &y, &width, &height);
        if (!flags || ((flags & WidthValue) && !width) ||
            ((flags & HeightValue) && !height)) {
            fprintf(stderr, "xdf: invalid geometry: %s\n", options.geometry);
            XCloseDisplay(display);
            return EXIT_FAILURE;
        }
        if (flags & XNegative) x += DisplayWidth(display, screen) - (int)width;
        if (flags & YNegative) y += DisplayHeight(display, screen) - (int)height;
    }
    IconColors icon_colors;
    unsigned long fg = color_pixel(display, options.foreground, &icon_colors.foreground);
    unsigned long bg = color_pixel(display, options.background, &icon_colors.background);
    unsigned long hl = color_pixel(display, options.highlight, &icon_colors.highlight);
    Window window = XCreateSimpleWindow(display, RootWindow(display, screen),
                                         x, y, width, height, 1, fg, bg);
    XStoreName(display, window, "xdf");
    XSelectInput(display, window, ExposureMask | StructureNotifyMask);
    XClassHint class_hint = { .res_name = "xdf", .res_class = "Xdf" };
    XSetClassHint(display, window, &class_hint);
    set_icon(display, window, &icon_colors);
    Atom wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &wm_delete, 1);

    Graph graph = { .display = display, .window = window, .width = width,
                    .height = height, .options = &options };
    graph.foreground = make_gc(display, window, fg);
    graph.background = make_gc(display, window, bg);
    graph.highlight = make_gc(display, window, hl);
    graph.font = XLoadQueryFont(display, "fixed");
    if (!graph.font) {
        fprintf(stderr, "xdf: cannot load the fixed font\n");
        return EXIT_FAILURE;
    }
    XSetFont(display, graph.foreground, graph.font->fid);
    resize_history(&graph, width);
    append_sample(&graph, percentage);
    XMapWindow(display, window);

    long long next_sample = monotonic_ms() + (long long)options.update * 1000;
    for (;;) {
        while (XPending(display)) {
            XEvent event;
            XNextEvent(display, &event);
            if (event.type == DestroyNotify) goto done;
            if (event.type == ClientMessage &&
                (Atom)event.xclient.data.l[0] == wm_delete) goto done;
            if (event.type == ConfigureNotify) {
                graph.width = (unsigned int)event.xconfigure.width;
                graph.height = (unsigned int)event.xconfigure.height;
                resize_history(&graph, graph.width);
                draw(&graph);
            } else if (event.type == Expose && event.xexpose.count == 0) {
                draw(&graph);
            }
        }
        long long now = monotonic_ms();
        if (now >= next_sample) {
            if (disk_percentage(options.path, &percentage) == 0) {
                append_sample(&graph, percentage);
                draw(&graph);
            } else {
                fprintf(stderr, "xdf: %s: %s\n", options.path, strerror(errno));
            }
            next_sample = now + (long long)options.update * 1000;
            continue;
        }
        int timeout = (int)(next_sample - now);
        struct pollfd pfd = { .fd = ConnectionNumber(display), .events = POLLIN };
        if (poll(&pfd, 1, timeout) < 0 && errno != EINTR) {
            perror("xdf: poll");
            break;
        }
    }
done:
    free(graph.history);
    XFreeFont(display, graph.font);
    XFreeGC(display, graph.foreground);
    XFreeGC(display, graph.background);
    XFreeGC(display, graph.highlight);
    XCloseDisplay(display);
    return EXIT_SUCCESS;
}
