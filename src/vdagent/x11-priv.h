#ifndef VDAGENT_X11_PRIV
#define VDAGENT_X11_PRIV

#include <stdint.h>
#include <stdio.h>

#include <spice/vd_agent.h>

#include <X11/extensions/Xrandr.h>

#define MAX_SCREENS 16
/* Same as qxl_dev.h client_monitors_config.heads count */
#define MONITOR_SIZE_COUNT 64

struct monitor_size {
    int width;
    int height;
};

struct vdagent_x11 {
    Display *display;
    Window root_window[MAX_SCREENS];
    struct udscs_connection *vdagentd;
    int debug;
    int fd;
    int screen_count;
    int width[MAX_SCREENS];
    int height[MAX_SCREENS];
    int xrandr_event_base;
    /* resolution change state */
    struct {
        XRRScreenResources *res;
        XRROutputInfo **outputs;
        XRRCrtcInfo **crtcs;
        int min_width;
        int max_width;
        int min_height;
        int max_height;
        int num_monitors;
        struct monitor_size monitor_sizes[MONITOR_SIZE_COUNT];
        VDAgentMonitorsConfig *failed_conf;
    } randr;

    /* NB: we cache this assuming the driver isn't changed under our feet */
    int set_crtc_config_not_functional;

    int has_xrandr;
    int xrandr_major;
    int xrandr_minor;
    int has_xinerama;
    int dont_send_guest_xorg_res;
};

extern int (*vdagent_x11_prev_error_handler)(Display *, XErrorEvent *);
extern int vdagent_x11_caught_error;

void vdagent_x11_randr_init(struct vdagent_x11 *x11);
void vdagent_x11_send_daemon_guest_xorg_res(struct vdagent_x11 *x11,
                                            int update);
void vdagent_x11_randr_handle_root_size_change(struct vdagent_x11 *x11,
                                            int screen, int width, int height);
int vdagent_x11_randr_handle_event(struct vdagent_x11 *x11,
    XEvent event);
void vdagent_x11_set_error_handler(struct vdagent_x11 *x11,
    int (*handler)(Display *, XErrorEvent *));
int vdagent_x11_restore_error_handler(struct vdagent_x11 *x11);

#endif // VDAGENT_X11_PRIV
