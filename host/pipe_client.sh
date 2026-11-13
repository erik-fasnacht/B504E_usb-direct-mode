#!/bin/sh
# pipe_client.sh: host side of B504e USB Direct Mode, for bench testing.
#
# Waits for PIPE READY from the B504e on the UART, downloads test data over
# the modem's USB network interface (usb0), then sends PIPE DONE with the
# result. Serves both cloud-initiated and edge-initiated pipes.
#
# Usage:
#   pipe_client.sh                  wait for pipes and serve them
#   pipe_client.sh request [reason] ask the B504e for a pipe first (edge),
#                                   then keep serving
#
# Pressing user button SW5 on the CC93 DVK also requests a pipe (edge),
# simulating the edge deciding it needs USB Direct Mode. Set BUTTON_LINE=none
# to disable. The cccs-gs-demo service must be stopped, since it claims the
# user button: systemctl disable --now cccs-gs-demo
#
# Requires the B504e UART wired to the XBee socket (/dev/ttyXBee), the
# eg91-dhcp udev rule + service (so usb0 gets an address on its own), and the
# eg91-deauthorize udev rule.
#
# The modem's USB is kept deauthorized except during a pipe: while a host has
# it configured, the modem stops answering Device OS reliably. This script
# authorizes it at PIPE READY. When the download is done, it puts the modem
# back in its default USB mode over the modem's own AT port and reboots it,
# then sends PIPE DONE ... modem=restored. The modem drops off USB and the
# udev rule deauthorizes it when it comes back, and the B504e does a single
# reset into an ordinary normal boot. (The B504e can't restore the modem
# itself at that point: after the host has used the modem's USB, the modem
# stops answering the B504e's UART until it restarts.)
#
# Ctrl-C to stop.
#
# Copyright (c) 2026, erik-fasnacht
# SPDX-License-Identifier: BSD-3-Clause

PORT=${PORT:-/dev/ttyXBee}
BAUD=${BAUD:-115200}
IFACE=${IFACE:-usb0}
CHUNKS=${CHUNKS:-2}
CHUNK_BYTES=${CHUNK_BYTES:-52428800}   # 50 MB; Cloudflare refuses >= 100 MB
URL="https://speed.cloudflare.com/__down?bytes=${CHUNK_BYTES}"
IFACE_TIMEOUT=${IFACE_TIMEOUT:-120}    # seconds to wait for usb0 + address
WGET_TIMEOUT=${WGET_TIMEOUT:-30}       # seconds without data before wget gives up
NTP_SERVER=${NTP_SERVER:-pool.ntp.org}  # clock sync once usb0 is up; NTP_SERVER=none to skip
MODEM_AT_PORT=${MODEM_AT_PORT:-/dev/ttyUSB2}  # modem's AT port over USB, during a pipe
DRAIN_TIMEOUT=${DRAIN_TIMEOUT:-5}      # seconds of UART quiet when checking for a B504e restart
BUTTON_CHIP=${BUTTON_CHIP:-gpiochip3}  # SW5 on the CC93 DVK is GPIO4_IO23
BUTTON_LINE=${BUTTON_LINE:-23}

CR=$(printf '\r')
LAST_ID=""
REPLAY=""     # a line read early while checking for a restart, to handle next

log() {
    echo "$(date '+%H:%M:%S') $*"
}

# Seconds since boot. Used for timing because the wall clock can jump when
# it syncs over cellular in the middle of a download.
now() {
    cut -d. -f1 /proc/uptime
}

# Authorize (1) or deauthorize (0) the modem's USB, wherever it enumerated.
modem_usb() {
    found=0
    for f in /sys/bus/usb/devices/*/idVendor; do
        [ "$(cat "$f" 2>/dev/null)" = "2c7c" ] || continue
        dir=${f%/idVendor}
        echo "$1" > "$dir/authorized" 2>/dev/null && found=1
    done
    if [ "$found" -eq 1 ]; then
        log "modem USB $( [ "$1" = 1 ] && echo authorized || echo deauthorized )"
    else
        log "modem USB not found"
    fi
}

send() {
    log ">> $*"
    printf '%s\n' "$*" >&3
}

# Wait until usb0 has an IPv4 address and the default route uses it.
wait_for_iface() {
    i=0
    while [ "$i" -lt "$IFACE_TIMEOUT" ]; do
        if ip -4 addr show "$IFACE" 2>/dev/null | grep -q "inet " &&
           ip route 2>/dev/null | grep -q "^default.* dev $IFACE"; then
            return 0
        fi
        sleep 1
        i=$((i + 1))
    done
    return 1
}

# Request a pipe each time the button is pressed. Runs in the background.
button_watch() {
    log "button watcher on $BUTTON_CHIP line $BUTTON_LINE"
    while :; do
        ev=$(gpiomon --num-events 1 --falling-edge "$BUTTON_CHIP" "$BUTTON_LINE" 2>/dev/null)
        # gpiomon prints one line per event. No output means it was stopped
        # (Ctrl-C ends it cleanly) or couldn't start (line busy), not a press.
        if [ -z "$ev" ]; then
            log "button watcher stopped"
            return
        fi
        log "button pressed"
        send "PIPE REQ reason=button"
        sleep 2   # debounce
    done
}

cleanup() {
    modem_usb 0
    if [ -n "$BUTTON_PID" ]; then
        kill "$BUTTON_PID" 2>/dev/null
        pkill -f "gpiomon --num-events 1 --falling-edge $BUTTON_CHIP $BUTTON_LINE" 2>/dev/null
    fi
}

# Send one AT command on fd 4 (the modem's AT port) and wait for OK.
at_cmd() {
    printf '%s\r' "$1" >&4
    while read -r -t 5 resp <&4; do
        resp=${resp%"$CR"}
        case "$resp" in
            OK) return 0 ;;
            ERROR*|"+CME ERROR"*) return 1 ;;
        esac
    done
    return 1
}

# End of the pipe: put the modem back in its default USB mode and reboot it,
# over its USB AT port. Returns 0 if the modem accepted the change. The
# reboot applies the setting and gives the B504e a freshly started modem.
restore_modem() {
    if [ ! -e "$MODEM_AT_PORT" ]; then
        log "modem AT port $MODEM_AT_PORT not found, the B504e will restore the modem"
        return 1
    fi
    stty -F "$MODEM_AT_PORT" 115200 raw -echo 2>/dev/null
    if ! exec 4<>"$MODEM_AT_PORT"; then
        log "could not open $MODEM_AT_PORT, the B504e will restore the modem"
        return 1
    fi
    if at_cmd 'AT+QCFG="usbnet",0'; then
        log "modem set to default USB mode"
        # Reboot it now. It drops off USB, so don't wait for an answer.
        printf 'AT+CFUN=1,1\r' >&4
        log "modem rebooting"
        exec 4>&-
        return 0
    fi
    log "modem didn't accept default USB mode, the B504e will restore it"
    exec 4>&-
    return 1
}

# After a failure, check whether it happened because the B504e restarted
# pipe mode (for example after a reset). Its messages queue up on the UART
# while we download, so read whatever is waiting and look for MODE pipe.
# Returns 0 if it restarted. A PIPE READY that arrives after the restart is
# kept in REPLAY so the main loop serves it.
b504e_restarted() {
    restarted=1
    while read -r -t "$DRAIN_TIMEOUT" pending <&3; do
        pending=${pending%"$CR"}
        [ -z "$pending" ] && continue
        log "<< $pending"
        case "$pending" in
            "MODE pipe "*) restarted=0 ;;
            "PIPE READY "*) [ "$restarted" -eq 0 ] && REPLAY=$pending ;;
        esac
    done
    return $restarted
}

# A step of the job failed: report it, unless the B504e restarted pipe mode,
# in which case it will reopen the pipe and we serve it again.
job_failed() {
    if b504e_restarted; then
        log "B504e restarted pipe mode, waiting for it to reopen the pipe"
        LAST_ID=""
        return
    fi
    flag=""
    restore_modem && flag=" modem=restored"
    send "PIPE DONE result=failed$flag detail=$1"
}

# Set the wall clock over the pipe's connection, before the download starts.
# The host has no network outside a pipe, so its clock may be far off, and
# the system time service would otherwise correct it partway through the
# download, making the log timestamps jump. Also saves it to the hardware
# clock so later boots start closer to the right time. Download timing uses
# uptime, so it's correct either way.
sync_clock() {
    [ "$NTP_SERVER" = "none" ] && return
    before=$(date '+%Y-%m-%d %H:%M:%S')

    # The system time service syncs on its own once the network is up, so
    # give it a few seconds first.
    if command -v timedatectl >/dev/null 2>&1; then
        i=0
        while [ "$i" -lt 10 ]; do
            if [ "$(timedatectl show -p NTPSynchronized --value 2>/dev/null)" = "yes" ]; then
                hwclock -w 2>/dev/null
                log "clock synced (was $before)"
                return
            fi
            sleep 1
            i=$((i + 1))
        done
    fi

    # Fallback: a one-shot ntpd. Run in a subshell so a timeout doesn't print
    # job-control noise.
    if command -v ntpd >/dev/null 2>&1 &&
       ( timeout 20 ntpd -n -q -p "$NTP_SERVER" ) >/dev/null 2>&1; then
        hwclock -w 2>/dev/null
        log "clock synced with ntpd (was $before)"
        return
    fi
    log "clock not synced, continuing"
}

run_job() {
    id=$1
    log "pipe $id open"
    modem_usb 1
    log "waiting for $IFACE"

    if ! wait_for_iface; then
        job_failed "no $IFACE address after ${IFACE_TIMEOUT}s"
        return
    fi
    addr=$(ip -4 addr show "$IFACE" | sed -n 's/.*inet \([0-9.]*\).*/\1/p')
    log "$IFACE up at $addr"
    sync_clock

    start=$(now)
    n=1
    while [ "$n" -le "$CHUNKS" ]; do
        log "chunk $n/$CHUNKS: $CHUNK_BYTES bytes"
        cstart=$(now)
        if ! wget -q -T "$WGET_TIMEOUT" -O /dev/null "$URL"; then
            secs=$(( $(now) - start ))
            job_failed "chunk $n of $CHUNKS failed after ${secs}s"
            return
        fi
        log "chunk $n done in $(( $(now) - cstart ))s"
        n=$((n + 1))
    done

    secs=$(( $(now) - start ))
    [ "$secs" -lt 1 ] && secs=1
    total=$((CHUNKS * CHUNK_BYTES))
    mbps=$(awk -v b="$total" -v s="$secs" 'BEGIN { printf "%.1f", b * 8 / s / 1000000 }')
    # Done with the modem: put it back to normal before handing it back.
    flag=""
    restore_modem && flag=" modem=restored"
    send "PIPE DONE result=ok$flag detail=$((total / 1048576))MB in ${secs}s ${mbps}Mbps"
}

if [ ! -e "$PORT" ]; then
    echo "$PORT not found" >&2
    exit 1
fi

stty -F "$PORT" "$BAUD" raw -echo || exit 1
exec 3<>"$PORT"

# Start with the modem's USB deauthorized, whatever state it was left in.
modem_usb 0

BUTTON_PID=""
trap cleanup EXIT
trap 'exit 130' INT TERM
if [ "$BUTTON_LINE" != "none" ] && command -v gpiomon >/dev/null 2>&1; then
    button_watch &
    BUTTON_PID=$!
fi

if [ "$1" = "request" ]; then
    send "PIPE REQ reason=${2:-edge}"
fi

log "listening on $PORT"

handle_line() {
    case "$1" in
        "MODE pipe "*)
            # The B504e (re)started pipe mode. If it reset mid-pipe, it
            # reopens the same id, so serve that id again.
            LAST_ID=""
            ;;
        "PIPE READY "*)
            id=$(echo "$1" | sed -n 's/.* id=\([^ ]*\).*/\1/p')
            # READY repeats every 30 s, and repeats queue up during a
            # download, so only act on each id once.
            if [ -n "$id" ] && [ "$id" != "$LAST_ID" ]; then
                LAST_ID=$id
                run_job "$id"
            fi
            ;;
    esac
}

while read -r line <&3; do
    line=${line%"$CR"}
    [ -z "$line" ] && continue
    log "<< $line"
    handle_line "$line"

    # A PIPE READY read early while checking for a B504e restart.
    while [ -n "$REPLAY" ]; do
        next=$REPLAY
        REPLAY=""
        handle_line "$next"
    done
done
