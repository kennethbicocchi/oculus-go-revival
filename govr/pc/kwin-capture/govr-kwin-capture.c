// govr-kwin-capture: ask KWin for a PipeWire screencast stream without the portal dialog.
//
// Uses KWin's privileged zkde_screencast_unstable_v1 protocol. KWin only exposes it to
// executables listed in a .desktop file with
//   X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1
// (see scripts/install-kwin-capture.sh).
//
// Usage:
//   govr-kwin-capture --list
//   govr-kwin-capture --output DP-3 [--no-cursor]
//   govr-kwin-capture --virtual 1920x1080 [--name GoVR]
// Prints "NODE <pipewire node id>" on stdout once the stream exists, then keeps the stream
// alive until stdin reaches EOF or the process is terminated.

#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

#include "zkde-screencast-unstable-v1-client-protocol.h"

#define MAX_OUTPUTS 16

struct output {
    struct wl_output* wl;
    char name[64];
    char desc[128];
    int width, height;
};

static struct zkde_screencast_unstable_v1* screencast;
static struct output outputs[MAX_OUTPUTS];
static int n_outputs;
static int done, failed;

static void out_geometry(void* d, struct wl_output* o, int32_t x, int32_t y, int32_t pw, int32_t ph,
                         int32_t sub, const char* make, const char* model, int32_t t) {}
static void out_mode(void* d, struct wl_output* o, uint32_t flags, int32_t w, int32_t h, int32_t r) {
    struct output* out = d;
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        out->width = w;
        out->height = h;
    }
}
static void out_done(void* d, struct wl_output* o) {}
static void out_scale(void* d, struct wl_output* o, int32_t s) {}
static void out_name(void* d, struct wl_output* o, const char* name) {
    snprintf(((struct output*)d)->name, sizeof(((struct output*)d)->name), "%s", name);
}
static void out_description(void* d, struct wl_output* o, const char* desc) {
    snprintf(((struct output*)d)->desc, sizeof(((struct output*)d)->desc), "%s", desc);
}
static const struct wl_output_listener output_listener = {
    out_geometry, out_mode, out_done, out_scale, out_name, out_description,
};

static void reg_global(void* d, struct wl_registry* reg, uint32_t id, const char* iface, uint32_t ver) {
    if (!strcmp(iface, zkde_screencast_unstable_v1_interface.name)) {
        screencast = wl_registry_bind(reg, id, &zkde_screencast_unstable_v1_interface, ver < 5 ? ver : 5);
    } else if (!strcmp(iface, wl_output_interface.name) && n_outputs < MAX_OUTPUTS && ver >= 4) {
        struct output* o = &outputs[n_outputs++];
        o->wl = wl_registry_bind(reg, id, &wl_output_interface, 4);
        wl_output_add_listener(o->wl, &output_listener, o);
    }
}
static void reg_remove(void* d, struct wl_registry* reg, uint32_t id) {}
static const struct wl_registry_listener registry_listener = {reg_global, reg_remove};

static void st_closed(void* d, struct zkde_screencast_stream_unstable_v1* s) {
    printf("CLOSED\n");
    fflush(stdout);
    done = 1;
}
static void st_created(void* d, struct zkde_screencast_stream_unstable_v1* s, uint32_t node) {
    printf("NODE %u\n", node);
    fflush(stdout);
}
static void st_failed(void* d, struct zkde_screencast_stream_unstable_v1* s, const char* err) {
    printf("FAILED %s\n", err);
    fflush(stdout);
    failed = done = 1;
}
static void st_serial(void* d, struct zkde_screencast_stream_unstable_v1* s, uint32_t hi, uint32_t lo) {
    printf("SERIAL %llu\n", ((unsigned long long)hi << 32) | lo);
    fflush(stdout);
}
static const struct zkde_screencast_stream_unstable_v1_listener stream_listener = {
    st_closed, st_created, st_failed, st_serial,
};

static void on_signal(int sig) { done = 1; }

int main(int argc, char** argv) {
    const char* want_output = NULL;
    const char* virt = NULL;
    const char* vname = "GoVR";
    int list = 0;
    uint32_t pointer = ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_EMBEDDED;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--list")) list = 1;
        else if (!strcmp(argv[i], "--output") && i + 1 < argc) want_output = argv[++i];
        else if (!strcmp(argv[i], "--virtual") && i + 1 < argc) virt = argv[++i];
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) vname = argv[++i];
        else if (!strcmp(argv[i], "--no-cursor")) pointer = ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_HIDDEN;
        else { fprintf(stderr, "unknown argument %s\n", argv[i]); return 2; }
    }
    struct wl_display* dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "cannot connect to the Wayland display\n"); return 1; }
    struct wl_registry* reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &registry_listener, NULL);
    wl_display_roundtrip(dpy);
    wl_display_roundtrip(dpy);

    if (list) {
        for (int i = 0; i < n_outputs; i++)
            printf("%s %dx%d %s\n", outputs[i].name, outputs[i].width, outputs[i].height, outputs[i].desc);
        printf("screencast %s\n", screencast ? "AVAILABLE" : "NOT AUTHORIZED");
        return 0;
    }
    if (!screencast) {
        printf("FAILED zkde_screencast_unstable_v1 not exposed to this executable "
               "(missing .desktop authorization, run scripts/install-kwin-capture.sh)\n");
        return 3;
    }

    struct zkde_screencast_stream_unstable_v1* stream = NULL;
    if (virt) {
        int w = 0, h = 0;
        if (sscanf(virt, "%dx%d", &w, &h) != 2) { fprintf(stderr, "bad size %s\n", virt); return 2; }
        stream = zkde_screencast_unstable_v1_stream_virtual_output(
            screencast, vname, w, h, wl_fixed_from_double(1.0), pointer);
    } else {
        struct output* target = n_outputs ? &outputs[0] : NULL;
        if (want_output) {
            target = NULL;
            for (int i = 0; i < n_outputs; i++)
                if (!strcmp(outputs[i].name, want_output)) target = &outputs[i];
        }
        if (!target) { printf("FAILED no such output %s\n", want_output ? want_output : "(none)"); return 4; }
        fprintf(stderr, "streaming output %s (%dx%d)\n", target->name, target->width, target->height);
        printf("SIZE %d %d\n", target->width, target->height);
        stream = zkde_screencast_unstable_v1_stream_output(screencast, target->wl, pointer);
    }
    zkde_screencast_stream_unstable_v1_add_listener(stream, &stream_listener, NULL);
    wl_display_flush(dpy);

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    struct pollfd fds[2] = {{wl_display_get_fd(dpy), POLLIN, 0}, {STDIN_FILENO, POLLIN, 0}};
    while (!done) {
        while (wl_display_prepare_read(dpy) != 0) wl_display_dispatch_pending(dpy);
        wl_display_flush(dpy);
        if (poll(fds, 2, 1000) < 0) { wl_display_cancel_read(dpy); break; }
        if (fds[0].revents & POLLIN) {
            if (wl_display_read_events(dpy) < 0) break;
        } else {
            wl_display_cancel_read(dpy);
        }
        wl_display_dispatch_pending(dpy);
        if (fds[1].revents & (POLLIN | POLLHUP)) {
            char buf[256];
            if (read(STDIN_FILENO, buf, sizeof(buf)) <= 0) break;  // parent went away
        }
        if (fds[0].revents & (POLLERR | POLLHUP)) break;
    }
    zkde_screencast_stream_unstable_v1_close(stream);
    wl_display_roundtrip(dpy);
    wl_display_disconnect(dpy);
    return failed ? 5 : 0;
}
