/* inkvt - VT100 terminal for E-ink devices
 * Copyright (C) 2020 Lennart Landsmeer <lennart@landsmeer.email>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sys/timerfd.h>
#include <signal.h>
#include <unistd.h>
#include <dirent.h>
#include <poll.h>
#include <linux/input.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/signalfd.h>

#include "setup_serial.hpp"
#include "buffers.hpp"
#include "insecure_http.hpp"
#include "vterm.hpp"

class Inputs {
public:
    Server server;
    bool had_input = 0;

    enum contact_tool {
        UNKNOWN_TOOL,
        FINGER,
        PEN,
    };
    enum contact_state {
        UNKNOWN_STATE,
        DOWN,
        UP,
    };

    struct {
        int32_t x = 0;
        int32_t y = 0;
        int32_t prev_x = 0;
        int32_t prev_y = 0;
        contact_tool tool = UNKNOWN_TOOL;
        contact_state state = UNKNOWN_STATE;
        bool moved = false;
    } istate;
private:
    enum fdtype {
        FD_EVDEV,
        FD_SERIAL,
        FD_PROGOUT,
        FD_SERVER,
        FD_SIGNAL,
        FD_STDIN,
        FD_VTERM_TIMER,
        FD_TIMER_NO_INPUT,
    };
    fdtype fdtype[128];
    struct pollfd fds[128];
    int nfds = 0;
    bool should_reset_termios = 0;
    struct termios termios_reset = {};
    VTermToFBInk * vterm = 0;

    bool handle_evdev(Buffers & buffers __attribute__((unused)), struct input_event * ev) {
        // NOTE: Lifted from https://github.com/NiLuJe/FBInk/blob/master/utils/finger_trace.c
        // NOTE: Shitty minimal state machinesque: we don't handle slots, gestures, or whatever ;).
        if (ev->type == EV_SYN && ev->code == SYN_REPORT) {
            // We only do stuff on each REPORT,
            // iff the finger actually moved somewhat significantly...
            // NOTE: Should ideally be clamped to between 0 and the relevant screen dimension ;).
            if ((istate.x > istate.prev_x + 2 ||
                istate.x < istate.prev_x - 2) ||
                (istate.y > istate.prev_y + 2 ||
                istate.y < istate.prev_y - 2)) {
                    istate.prev_x = istate.x;
                    istate.prev_y = istate.y;
                    istate.moved = true;
            }

            // Keep draining the queue without going back to poll
            return true;
        }

        // Detect tool type & all contacts up on Mk. 7 (and possibly earlier "snow" protocol devices).
        if (ev->type == EV_KEY) {
            switch (ev->code) {
                case BTN_TOOL_PEN:
                    istate.tool = PEN;
                    if (ev->value > 0) {
                        istate.state = DOWN;
                    } else {
                        istate.state = UP;
                    }
                    break;
                case BTN_TOOL_FINGER:
                    istate.tool = FINGER;
                    if (ev->value > 0) {
                        istate.state = DOWN;
                    } else {
                        istate.state = UP;
                    }
                    break;
                case BTN_TOUCH:
                    // To detect up/down state on "snow" protocol without weird slot shenanigans...
                    // It's out-of-band of MT events, so, it unfortunately means *all* contacts,
                    // not a specific slot...
                    // (i.e., you won't get an EV_KEY:BTN_TOUCH:0 until *all* contact points have been lifted).
                    if (ev->value > 0) {
                        istate.state = DOWN;
                    } else {
                        istate.state = UP;
                    }
                    break;
            }
        }

        if (ev->type == EV_ABS) {
            switch (ev->code) {
                case ABS_MT_TOOL_TYPE:
                    // Detect tool type on Mk. 8
                    if (ev->value == 0) {
                        istate.tool = FINGER;
                    } else if (ev->value == 1) {
                        istate.tool = PEN;
                    }
                    break;
                // NOTE: That should cover everything...
                //       Mk. 6+ reports EV_KEY:BTN_TOUCH events,
                //       which would be easier to deal with,
                //       but redundant here ;).
                // NOTE: When in doubt about what a simple event stream looks like on specific devices,
                //       check generate_button_press @ fbink_button_scan.c ;).
                case ABS_PRESSURE:
                case ABS_MT_WIDTH_MAJOR:
                //case ABS_MT_TOUCH_MAJOR: // Oops, not that one, it's always 0 on early Mk.7 devices :s
                case ABS_MT_PRESSURE:
                    if (ev->value > 0) {
                        istate.state = DOWN;
                    } else {
                        istate.state = UP;
                    }
                    break;
                case ABS_X:
                case ABS_MT_POSITION_X:
                    istate.x = ev->value;
                    break;
                case ABS_Y:
                case ABS_MT_POSITION_Y:
                    istate.y = ev->value;
                    break;
                case ABS_MT_TRACKING_ID:
                    if (ev->value == -1) {
                        istate.state = UP;
                    }
                    // NOTE: Could also be used for sunxi pen mode shenanigans
                    break;
                default:
                    break;
            }
        }

        return false;
    }

    void handle_evdev(Buffers & buffers, int fd) {
        struct input_event ev;
        // Drain the full input frame in one go
        for (;;) {
            ssize_t nread = read(fd, &ev, sizeof(struct input_event));
            if (nread != sizeof(struct input_event)) {
                break;
            }
            handle_evdev(buffers, &ev);
        }
    }

    void handle_serial(Buffers & buffers, int fd) {
        char buf[1];
        for (;;) {
            ssize_t nread = read(fd, buf, sizeof(buf));
            if (nread == -1 || nread == 0) { // errno = EAGAIN for blocking read
                break;
            }
            for (ssize_t n = 0; n < nread; n++) {
                buffers.serial.push_back(buf[n]);
            }
        }
    }

    void handle_progout(Buffers & buffers, int fd) {
        char buf[64];
        ssize_t nread = read(fd, buf, sizeof(buf));
        if (nread < 0) return;
        for (ssize_t i = 0; i < nread; i++) {
            buffers.vt100_in.push_back(buf[i]);
        }
        // NOTE: Don't read out everything available
        // That would mean blocking in this function,
        // which disables receiving signals
        // poll() will call us again if there is more data
    }

    void handle_server(Buffers & buffers, int fd) {
        if (fd != server.fd) return;
        server.accept(buffers.keyboard);
    }

    void handle_vterm_timer(Buffers & buffers __attribute__((unused)), int fd) {
        uint64_t buf;
        if (read(fd, &buf, sizeof(buf)) > 0) {
            vterm->tick();
        }
    }

    void handle_signal(Buffers & buffers, int fd) {
        struct signalfd_siginfo fdsi;
        ssize_t s = read(fd, &fdsi, sizeof(struct signalfd_siginfo));
        if (s != sizeof(fdsi)) return;
        if (fdsi.ssi_signo == SIGINT) {
            buffers.keyboard.push_back(0x03);
        } else {
            printf("Got signal %u, exiting now\n", fdsi.ssi_signo);
            exit(EXIT_SUCCESS);
        }
    }

    void handle_stdin(Buffers & buffers, int fd) {
        char c;
        while(read(fd, &c, 1) == 1) {
            buffers.keyboard.push_back(c);
        }
    }

    void handle_input_timeout(Buffers & buffers __attribute__((unused)), int fd) {
        uint64_t buf;
        if (read(fd, &buf, sizeof(buf)) > 0 && !had_input) {
            printf("input timeout\n");
            exit(1);
        }
    }

public:
    void wait(Buffers & buffers) {
        poll(fds, nfds, -1);
        for (int i = 0; i < nfds; i++) {
            if (!fds[i].revents) {
                continue;
            }
            if (fdtype[i] == FD_EVDEV) {
                handle_evdev(buffers, fds[i].fd);
            } else if (fdtype[i] == FD_SERIAL) {
                handle_serial(buffers, fds[i].fd);
            } else if (fdtype[i] == FD_PROGOUT) {
                if (fds[i].revents & POLLHUP) {
                    // pty slave disconnected
                    exit(0);
                } else {
                    handle_progout(buffers, fds[i].fd);
                }
            } else if (fdtype[i] == FD_SERVER) {
                handle_server(buffers, fds[i].fd);
            } else if (fdtype[i] == FD_SIGNAL) {
                handle_signal(buffers, fds[i].fd);
            } else if (fdtype[i] == FD_STDIN) {
                handle_stdin(buffers, fds[i].fd);
            } else if (fdtype[i] == FD_VTERM_TIMER) {
                handle_vterm_timer(buffers, fds[i].fd);
            } else if (fdtype[i] == FD_TIMER_NO_INPUT) {
                handle_input_timeout(buffers, fds[i].fd);
            }
        }
    }

    void add_evdev() {
#ifdef TARGET_KOBO
        size_t dev_count;
        FBInkInputDevice* devices = fbink_input_scan(INPUT_TOUCHSCREEN, 0U, 0U, &dev_count);
        if (devices) {
            for (FBInkInputDevice* device = devices; device < devices + dev_count; device++) {
                if (device->matched) {
                    fdtype[nfds] = FD_EVDEV;
                    fds[nfds].events = POLLIN;
                    fds[nfds++].fd = device->fd;
                    printf("Opened touch input device `%s` @ `%s`\n", device->name, device->path);
                }
            }
            free(devices);
        }
#else
        struct dirent **namelist;
        int ndev = scandir("/dev/input", &namelist, &_is_event_device, alphasort);
        for (int i = 0; i < ndev; i++) {
            char fname[512];
            snprintf(fname, sizeof(fname), "/dev/input/%s", namelist[i]->d_name);
            int fd = open(fname, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd == -1) {
                printf("couldn't open %s %d\n", fname, fd);
                continue;
            }
            if (0 && !ioctl(fd, EVIOCGRAB, (void*)1)) {
                close(fd);
                continue;
            }
            printf("opened %s\n", fname);
            fdtype[nfds] = FD_EVDEV;
            fds[nfds].events = POLLIN;
            fds[nfds++].fd = fd;
        }
#endif
    }

    void add_progout(int fd) {
        fdtype[nfds] = FD_PROGOUT;
        fds[nfds].events = POLLIN | POLLHUP;
        fds[nfds++].fd = fd;
    }

    void add_vterm_timer(int fd, VTermToFBInk * vt) {
        fdtype[nfds] = FD_VTERM_TIMER;
        fds[nfds].events = POLLIN;
        fds[nfds++].fd = fd;
        this->vterm = vt;
    }

    bool add_serial() {
#ifdef TARGET_KOBO
        // NOTE: Do our best to make this mostly portable...
        //       See http://trac.ak-team.com/trac/browser/niluje/Configs/trunk/Kindle/Kobo_Hacks/KoboStuff/src/usr/local/stuff/bin/usbnet-toggle.sh for a similar example.
        // NOTE: Fun fact: I don't know when Kobo started shipping g_serial, but they didn't on Mk.5, so, here's one I just built to test on my H2O:
        //       http://files.ak-team.com/niluje/mrpub/Other/USBSerial-Kobo-Mk5-H2O.tar.gz
        const char *platform = getenv("PLATFORM");

        // Abort if we can't tell the platform from the env.
        // NOTE: We could also compute that from FBInk's state via device_platform, although that's not a 1:1 mapping...
        if (platform == nullptr) {
            puts("add_serial() is only supported on Kobo devices with a proper PLATFORM set in the env!");
            return false;
        }

        char module_path[PATH_MAX] = { 0 };

        snprintf(module_path, sizeof(module_path), "/drivers/%s/usb/gadget/g_serial.ko", platform);
        if (access(module_path, F_OK) != 0) {
            puts("add_serial() is only supported on Kobo devices with a g_serial kernel module!");
            return false;
        }

        // Cheap Mk. 7+ detection...
        snprintf(module_path, sizeof(module_path), "/drivers/%s/usb/gadget/configfs.ko", platform);
        if (access(module_path, F_OK) == 0) {
            // Mk. 7+
            snprintf(module_path, sizeof(module_path), "insmod /drivers/%s/usb/gadget/configfs.ko", platform);
            (void)system(module_path);
            snprintf(module_path, sizeof(module_path), "insmod /drivers/%s/usb/gadget/libcomposite.ko", platform);
            (void)system(module_path);
            snprintf(module_path, sizeof(module_path), "insmod /drivers/%s/usb/gadget/u_serial.ko", platform);
            (void)system(module_path);
            snprintf(module_path, sizeof(module_path), "insmod /drivers/%s/usb/gadget/usb_f_acm.ko", platform);
            (void)system(module_path);
            snprintf(module_path, sizeof(module_path), "insmod /drivers/%s/usb/gadget/g_serial.ko", platform);
            (void)system(module_path);
        } else {
            // Older devices
            snprintf(module_path, sizeof(module_path), "insmod /drivers/%s/usb/gadget/arcotg_udc.ko", platform);
            (void)system(module_path);
            snprintf(module_path, sizeof(module_path), "insmod /drivers/%s/usb/gadget/g_serial.ko", platform);
            (void)system(module_path);
        }

        // Sleep a bit to leave the kernel time to breathe, because everything is terrible
        const struct timespec zzz   = { 0L, 500000000L };
        nanosleep(&zzz, nullptr);

        int fd = open("/dev/ttyGS0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd != -1) {
            fdtype[nfds] = FD_SERIAL;
            fds[nfds].events = POLLIN;
            fds[nfds++].fd = fd;
            puts("opening /dev/ttyGS0");
            setup_serial(fd);
            return true;
        } else {
            printf("couldn't open /dev/ttyGS0: %m\n");
            return false;
        }
#else
        puts("add_serial() is only supported on Kobo devices");
#endif
        return false;
    }

    int add_http(uint16_t port) {
        if (server.setup(port) < -1) {
            return -1;
        }
        fdtype[nfds] = FD_SERVER;
        fds[nfds++] = server.get_pollfd();
        return 0;
    }

    bool is_listening_on_http() {
        return server.fd != -1;
    }

    void add_ttyraw() {
        should_reset_termios = 1;
        tcgetattr(STDIN_FILENO, &termios_reset);
        struct termios raw = termios_reset;
        raw.c_lflag &= ~(ECHO | ICANON);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
        int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
        fdtype[nfds] = FD_STDIN;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        fds[nfds++].fd = STDIN_FILENO;
    }

    void add_signals(std::vector<int> signals) {
        sigset_t mask = { 0 };
        sigemptyset(&mask);
        for (int signal : signals) {
            if (sigaddset(&mask, signal) != 0) {
                puts("sigaddset");
                exit(1);
            }
        }
        if (sigprocmask(SIG_BLOCK, &mask, 0) == -1) {
            perror("sigprocmask");
            exit(1);
        }
        int fd = signalfd(-1, &mask, 0);
        if (fd == -1) {
            perror("signalfd");
            exit(1);
        }
        fdtype[nfds] = FD_SIGNAL;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        fds[nfds++].fd = fd;
    }

    void add_signals() {
        add_signals({SIGINT, SIGQUIT});
    }

    void atexit() {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &termios_reset);
    }

    void add_exit_after(int seconds) {
        itimerspec ts;
        int timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
        if (timerfd < 0) {
            perror("add_exit_after:timerfd_create");
            exit(1);
        }
        ts.it_value.tv_sec = seconds;
        ts.it_value.tv_nsec = 0;
        ts.it_interval.tv_sec = 0;
        ts.it_interval.tv_nsec = 0;
        long err = timerfd_settime(timerfd, 0, &ts, 0);
        if (err < 0) {
            perror("add_exit_after:timerfd_settime");
            exit(1);
        }
        fdtype[nfds] = FD_TIMER_NO_INPUT;
        fds[nfds].events = POLLIN;
        fds[nfds++].fd = timerfd;
    }

};
