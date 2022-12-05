#!/bin/sh
# From https://github.com/koreader/koreader/blob/master/platform/kobo/koreader.sh
# Copyright (C) 2020-2022 KOReader
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.


export LC_ALL="en_US.UTF-8"

# Compute our working directory in an extremely defensive manner
INKVT_DIR="$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd -P)"

# We rely on starting from our working directory, and it needs to be set, sane and absolute.
cd "${INKVT_DIR:-/dev/null}" || exit

# Quick'n dirty way of checking if we were started while Nickel was running (e.g., KFMon),
# or from another launcher entirely, outside of Nickel (e.g., KSM).
VIA_NICKEL="false"
if pkill -0 nickel; then
    VIA_NICKEL="true"
fi

if [ "${VIA_NICKEL}" = "true" ]; then
    # Detect if we were started from KFMon
    FROM_KFMON="false"
    if pkill -0 kfmon; then
        # That's a start, now check if KFMon truly is our parent...
        if [ "$(pidof -s kfmon)" -eq "${PPID}" ]; then
            FROM_KFMON="true"
        fi
    fi

    # Check if Nickel is our parent...
    FROM_NICKEL="false"
    if [ -n "${NICKEL_HOME}" ]; then
        FROM_NICKEL="true"
    fi

    # If we were spawned outside of Nickel, we'll need a few extra bits from its own env...
    if [ "${FROM_NICKEL}" = "false" ]; then
        # Siphon a few things from nickel's env (namely, stuff exported by rcS *after* on-animator.sh has been launched)...
        # shellcheck disable=SC2046
        export $(grep -s -E -e '^(DBUS_SESSION_BUS_ADDRESS|NICKEL_HOME|WIFI_MODULE|LANG|INTERFACE)=' "/proc/$(pidof -s nickel)/environ")
        # NOTE: Quoted variant, w/ the busybox RS quirk (c.f., https://unix.stackexchange.com/a/125146):
        #eval "$(awk -v 'RS="\0"' '/^(DBUS_SESSION_BUS_ADDRESS|NICKEL_HOME|WIFI_MODULE|LANG|INTERFACE)=/{gsub("\047", "\047\\\047\047"); print "export \047" $0 "\047"}' "/proc/$(pidof -s nickel)/environ")"
    fi

    # Flush disks, might help avoid trashing nickel's DB...
    sync
    # And we can now stop the full Kobo software stack
    # NOTE: We don't need to kill KFMon, it's smart enough not to allow running anything else while we're up
    # NOTE: We kill Nickel's master dhcpcd daemon on purpose,
    #       as we want to be able to use our own per-if processes w/ custom args later on.
    #       A SIGTERM does not break anything, it'll just prevent automatic lease renewal until the time
    #       KOReader actually sets the if up itself (i.e., it'll do)...
    killall -q -TERM nickel hindenburg sickel fickel strickel fontickel adobehost foxitpdf iink dhcpcd-dbus dhcpcd bluealsa bluetoothd fmon nanoclock.lua

    # Wait for Nickel to die... (oh, procps with killall -w, how I miss you...)
    kill_timeout=0
    while pkill -0 nickel; do
        # Stop waiting after 4s
        if [ ${kill_timeout} -ge 15 ]; then
            break
        fi
        usleep 250000
        kill_timeout=$((kill_timeout + 1))
    done
    # Remove Nickel's FIFO to avoid udev & udhcpc scripts hanging on open() on it...
    rm -f /tmp/nickel-hardware-status
else
    echo Not running from Nickel/kfmon!
    echo In general, there could be some interference between whatever is running and inkvt
    echo You should run the inkvt binary directly in this case
    exit 1
fi

# check whether PLATFORM & PRODUCT have a value assigned by rcS
if [ -z "${PRODUCT}" ]; then
    # shellcheck disable=SC2046
    export $(grep -s -e '^PRODUCT=' "/proc/$(pidof -s udevd)/environ")
fi

if [ -z "${PRODUCT}" ]; then
    PRODUCT="$(/bin/kobo_config.sh 2>/dev/null)"
    export PRODUCT
fi

# PLATFORM is used in koreader for the path to the Wi-Fi drivers (as well as when restarting nickel)
if [ -z "${PLATFORM}" ]; then
    # shellcheck disable=SC2046
    export $(grep -s -e '^PLATFORM=' "/proc/$(pidof -s udevd)/environ")
fi

if [ -z "${PLATFORM}" ]; then
    PLATFORM="freescale"
    if dd if="/dev/mmcblk0" bs=512 skip=1024 count=1 | grep -q "HW CONFIG"; then
        CPU="$(ntx_hwconfig -s -p /dev/mmcblk0 CPU 2>/dev/null)"
        PLATFORM="${CPU}-ntx"
    fi

    if [ "${PLATFORM}" != "freescale" ] && [ ! -e "/etc/u-boot/${PLATFORM}/u-boot.mmc" ]; then
        PLATFORM="ntx508"
    fi
    export PLATFORM
fi

# Make sure we have a sane-ish INTERFACE env var set...
if [ -z "${INTERFACE}" ]; then
    # That's what we used to hardcode anyway
    INTERFACE="eth0"
    export INTERFACE
fi

if [ -e crash.log ]; then
    tail -c 500000 crash.log >crash.log.new
    mv -f crash.log.new crash.log
fi

# We'll want to ensure Portrait rotation to allow us to use faster blitting codepaths @ 8bpp,
# so remember the current one before fbdepth does its thing.
IFS= read -r ORIG_FB_ROTA <"/sys/class/graphics/fb0/rotate"
echo "Original fb rotation is set @ ${ORIG_FB_ROTA}" >>crash.log 2>&1

# In the same vein, swap to 8bpp,
# because 16bpp is the worst idea in the history of time, as RGB565 is generally a PITA without hardware blitting,
# and 32bpp usually gains us nothing except a performance hit (we're not Qt5 with its QPainter constraints).
# The reduced size & complexity should hopefully make things snappier,
# (and hopefully prevent the JIT from going crazy on high-density screens...).
# NOTE: Even though both pickel & Nickel appear to restore their preferred fb setup, we'll have to do it ourselves,
#       as they fail to flip the grayscale flag properly. Plus, we get to play nice with every launch method that way.
#       So, remember the current bitdepth, so we can restore it on exit.
IFS= read -r ORIG_FB_BPP <"/sys/class/graphics/fb0/bits_per_pixel"
echo "Original fb bitdepth is set @ ${ORIG_FB_BPP}bpp" >>crash.log 2>&1
# Sanity check...
case "${ORIG_FB_BPP}" in
    8) ;;
    16) ;;
    32) ;;
    *)
        # Uh oh? Don't do anything...
        unset ORIG_FB_BPP
        ;;
esac

# On sunxi, the fb state is meaningless, and the minimal disp fb doesn't actually support 8bpp anyway...
if [ "${PLATFORM}" = "b300-ntx" ]; then
    # NOTE: The fb state is *completely* meaningless on this platform.
    #       This is effectively a noop, we're just keeping it for logging purposes...
    echo "Making sure that rotation is set to Portrait" >>crash.log 2>&1
    ./fbdepth -R UR >>crash.log 2>&1
    # We haven't actually done anything, so don't do anything on exit either ;).
    unset ORIG_FB_BPP

    return
fi
# Swap to 8bpp if things looke sane
if [ -n "${ORIG_FB_BPP}" ]; then
    echo "Switching fb bitdepth to 8bpp & rotation to Portrait" >>crash.log 2>&1
    ./fbdepth -d 8 -R UR >>crash.log 2>&1
fi

# Ensure we start with a valid nameserver in resolv.conf, otherwise we're stuck with broken name resolution (#6421, #6424).
# Fun fact: this wouldn't be necessary if Kobo were using a non-prehistoric glibc... (it was fixed in glibc 2.26).
# If there aren't any servers listed, append CloudFlare's
if ! grep -q '^nameserver' "/etc/resolv.conf"; then
    echo "# Added by KOReader because your setup is broken" >>"/etc/resolv.conf"
    echo "nameserver 1.1.1.1" >>"/etc/resolv.conf"
fi

# VT100 terminal for E-ink devices
# Usage:
#   inkvt [OPTION...]
#
#   -h, --help          Print usage
#       --no-reinit     Do not issue fbink_reinit() calls (assume no
#                       plato/nickel running)
#       --serial        Load g_serial and listen on serial (might break usbms
#                       until reboot)
#       --no-http       Do not listen on http
#       --no-timeout    Do not exit after 20 seconds of no input
#       --no-signals    Do not catch signals
#       --osk           Experimental OSK
#   -f, --fontname arg  FBInk Bitmap fontname, one of ibm, unscii, unscii_alt,
#                       unscii_thin, unscii_fantasy, unscii_mcr, unscii_tall,
#                       block, leggie, veggie, kates, fkp, ctrld, orp, orpb, orpi,
#                       scientifica, scientificab, scientificai, terminus,
#                       terminusb, fatty, spleen, tewi, tewib, topaz, microknight,
#                       vga or cozette (default: terminus)
#   -s, --fontsize arg  Fontsize multiplier (default: 2)

./inkvt.armhf --no-reinit --no-http --osk >> crash.log 2>&1
RETURN_VALUE=$?

# Restore original fb bitdepth if need be...
# Since we also (almost) always enforce Portrait, we also have to restore the original rotation no matter what ;).
if [ -n "${ORIG_FB_BPP}" ]; then
    echo "Restoring original fb bitdepth @ ${ORIG_FB_BPP}bpp & rotation @ ${ORIG_FB_ROTA}" >>crash.log 2>&1
    ./fbdepth -d "${ORIG_FB_BPP}" -r "${ORIG_FB_ROTA}" >>crash.log 2>&1
else
    echo "Restoring original fb rotation @ ${ORIG_FB_ROTA}" >>crash.log 2>&1
    ./fbdepth -r "${ORIG_FB_ROTA}" >>crash.log 2>&1
fi

if [ "${VIA_NICKEL}" = "true" ]; then
    if [ "${FROM_KFMON}" != "true" ]; then
        ./nickel.sh &
    else
        ./nickel.sh &
    fi
else
    if ! pkill -0 -f kbmenu; then
        /sbin/reboot
    fi
fi

exit ${RETURN_VALUE}
