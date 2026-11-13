/*
 * main.cpp
 *
 * Particle B504e firmware: UART telematics relay with on-demand USB Direct
 * Mode, which hands the cellular modem to a Linux host over USB for large
 * downloads (a "pipe").
 *
 *   Normal mode: connected to the Particle cloud, relaying telematics from
 *                the host over UART.
 *   Pipe mode:   offline, modem handed to the host until PIPE DONE or a
 *                timeout. Entered and exited with System.reset().
 *
 * See README.md for an overview and docs/PROTOCOL.md for the UART protocol,
 * ledger schemas and environment variables.
 *
 * Copyright (c) 2026, erik-fasnacht
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Requires Device OS 6.3.0+ (CloudEvent). Ledger needs 6.1.0+, environment
 * variables 6.4.0+ (compiled out on older targets).
 */

#include "Particle.h"
#include <cerrno>

SYSTEM_MODE(SEMI_AUTOMATIC);
SYSTEM_THREAD(ENABLED);

PRODUCT_VERSION(1);

SerialLogHandler logHandler(LOG_LEVEL_INFO);

// ---------------------------------------------------------------- settings

static const uint32_t DEFAULT_PIPE_MAX_MINUTES = 20;
static const uint32_t DEFAULT_PIPE_SETTLE_S = 60;
static const uint32_t DEFAULT_UART_BAUD = 115200;

static const uint32_t READY_REPEAT_MS = 30000;
static const uint32_t LEDGER_SYNC_TIMEOUT_MS = 35000;   // sync starts within 30 s
static const uint32_t MODEM_OFF_TIMEOUT_MS = 30000;      // modem power cycle within a pipe
static const uint32_t ENTER_PIPE_OFF_WAIT_MS = 15000;   // before resetting into pipe mode
static const uint32_t RESET_OFF_WAIT_MS = 5000;         // before other resets
static const uint8_t MAX_PIPE_BOOTS = 3;                // crash loop guard

// Modem USB mode (AT+QCFG="usbnet"): 0 = default (Device OS can dial),
// 1 = ECM (host gets a usb0 network interface via cdc_ether).
static const int USBNET_DEFAULT = 0;
static const int USBNET_ECM = 1;
//
// AT commands are only sent in pipe mode, at the end of a pipe, and in
// normal mode only to clean up after a pipe that ended uncleanly. A clean
// normal boot sends none, so it behaves like a stock device.
//
// Timing in pipe mode follows what worked in testing: query early (~5 s
// after boot, before Device OS starts recovering a modem it finds still in
// multiplexer mode after a reset), then every 10 s. Powering the modem off
// before a reset is attempted, but not relied on: on the bench, with the
// host's USB attached, the modem never reported off.
static const uint32_t USBNET_CMD_TIMEOUT_MS = 2000;     // per AT command
static const uint32_t USBNET_FIRST_QUERY_MS = 5000;     // after powering the modem on
static const uint32_t USBNET_RETRY_GAP_MS = 10000;      // between queries
static const uint32_t USBNET_ERROR_BACKOFF_MS = 10000;  // after the modem answers ERROR
static const uint32_t USBNET_READ_TIMEOUT_MS = 180000;  // then give up
static const uint8_t USBNET_MAX_SWITCHES = 2;

// Normal mode: if the cloud stays disconnected this long, reset. Covers any
// way of getting stuck offline (modem wedged, left in ECM, network trouble).
static const uint32_t OFFLINE_RESET_MS = 10 * 60 * 1000;

// Normal mode: if the cloud still isn't connected after this long, check
// whether the modem was left in ECM mode and fix it. Covers power loss
// mid-pipe, where the retained ECM marker (below) is lost too.
static const uint32_t FALLBACK_CHECK_MS = 3 * 60 * 1000;

// Hardware watchdog: if loop() stops running for this long, the B504e
// resets itself. A modem command can block for tens of seconds while Device
// OS is busy with the modem, so this leaves generous room.
static const std::chrono::milliseconds WATCHDOG_TIMEOUT = 120s;

static const size_t MAX_LINE = 600;
static const size_t MAX_FIELDS = 32;

static const char *REQUEST_LEDGER = "pipe-request";
static const char *STATUS_LEDGER = "pipe-status";

struct Config {
    uint32_t pipeMaxMinutes;
    uint32_t pipeSettleS;
    uint32_t uartBaud;
};

static Config cfg = {DEFAULT_PIPE_MAX_MINUTES, DEFAULT_PIPE_SETTLE_S, DEFAULT_UART_BAUD};

// ---------------------------------------------------------------- helpers

static void copyStr(char *dst, size_t n, const char *src)
{
    strncpy(dst, src ? src : "", n - 1);
    dst[n - 1] = '\0';
}

// Send one line to the host (Serial1) and mirror it to USB for debugging.
static void say(const char *fmt, ...)
{
    static char buf[MAX_LINE + 128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    Serial1.println(buf);
    Serial.println(buf);
}

static uint32_t envUint(const char *name, uint32_t def, uint32_t minV, uint32_t maxV)
{
#if HAL_PLATFORM_ENV
    int v = 0;
    if (System.getEnv(name, v)) {
        if (v >= (int)minV && v <= (int)maxV) {
            Log.info("env %s=%d", name, v);
            return (uint32_t)v;
        }
        Log.warn("env %s=%d out of range, using default %lu", name, v, (unsigned long)def);
    }
#endif
    return def;
}

static void loadConfig()
{
    cfg.pipeMaxMinutes = envUint("PIPE_MAX_MINUTES", DEFAULT_PIPE_MAX_MINUTES, 1, 240);
    cfg.pipeSettleS = envUint("PIPE_SETTLE_S", DEFAULT_PIPE_SETTLE_S, 0, 600);
    cfg.uartBaud = envUint("UART_BAUD", DEFAULT_UART_BAUD, 9600, 1000000);
}

// ------------------------------------------------- retained (pipe request)
//
// Survives System.reset() but not power loss. Validated with a magic number
// and checksum, so after power loss it reads as invalid and the device boots
// into normal mode: the safe failure.

static const uint32_t RET_MAGIC = 0x50495045;  // "PIPE"

struct PipeRetained {
    uint32_t magic;
    uint8_t requested;
    uint8_t fromCloud;
    uint8_t boots;
    uint8_t closing;    // pipe already finished; a reset now goes to normal mode
    char id[64];
    char reason[24];
    char job[512];
    uint32_t crc;
};

retained PipeRetained ret;

// Set while the modem may be in ECM mode, cleared once default mode is
// confirmed. Survives resets (not power loss), so a normal boot after a pipe
// that ended uncleanly (crash, watchdog, reset mid-close) restores default
// mode before connecting. A normal boot after a clean pipe sees it cleared
// and sends the modem no commands.
static const uint32_t ECM_MARKER = 0x45434D31;  // "ECM1"
retained uint32_t ecmMarker;

static void noteUsbnet(int m)
{
    ecmMarker = (m == USBNET_ECM) ? ECM_MARKER : 0;
}

static uint32_t retCrc()
{
    const uint8_t *p = (const uint8_t *)&ret;
    uint32_t h = 2166136261u;  // FNV-1a
    for (size_t i = 0; i < offsetof(PipeRetained, crc); i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static bool retValid()
{
    return ret.magic == RET_MAGIC && ret.crc == retCrc();
}

static void retSave()
{
    ret.magic = RET_MAGIC;
    ret.crc = retCrc();
}

static void retClear()
{
    memset(&ret, 0, sizeof(ret));
}

// ----------------------------------------------- EEPROM (survives power loss)
//
// The last completed pipe (reported in pipe-status) and the last cloud
// request id handled, so a re-synced pipe-request ledger is not re-run.

static const int EEPROM_ADDR = 0;
static const uint32_t PERSIST_MAGIC = 0x4C415354;  // "LAST"

struct LastPipe {
    char id[64];
    char reason[24];
    char result[16];
    char detail[96];
    uint32_t durationS;
};

struct Persist {
    uint32_t magic;
    char lastCloudId[64];
    LastPipe last;
};

static Persist persist;

static void loadPersist()
{
    EEPROM.get(EEPROM_ADDR, persist);
    if (persist.magic != PERSIST_MAGIC) {
        memset(&persist, 0, sizeof(persist));
        persist.magic = PERSIST_MAGIC;
    }
    persist.lastCloudId[sizeof(persist.lastCloudId) - 1] = '\0';
}

static void savePersist()
{
    persist.magic = PERSIST_MAGIC;
    EEPROM.put(EEPROM_ADDR, persist);
}

// ---------------------------------------------------------------- state

enum class Mode { NORMAL, PIPE };
static Mode mode = Mode::NORMAL;

// Normal mode: steps from "request accepted" to System.reset().
enum class Transition { NONE, WRITE_STATUS, WAIT_SYNC, NETWORK_OFF };
static Transition transition = Transition::NONE;
static uint32_t transitionStart = 0;

struct PendingPipe {
    String id;
    String reason;
    String job;
    bool fromCloud;
};
static PendingPipe pending;

// Pipe mode.
enum class PipePhase { PREP, SETTLING, OPEN };
static PipePhase pipePhase = PipePhase::PREP;
static uint32_t pipeBootMs = 0;
static uint32_t settleStartMs = 0;
static uint32_t openedMs = 0;
static uint32_t lastReadyMs = 0;


// Normal mode.
static bool normalPrep = false;     // restoring default mode before connecting
static uint32_t lastOnlineMs = 0;   // last time the cloud was connected
static bool fallbackStarted = false;
static bool fallbackActive = false;

static Ledger requestLedger;
static Ledger statusLedger;
static volatile bool requestSynced = false;
static bool statusReported = false;
static uint32_t connectedAt = 0;
static bool cachedRequestChecked = false;

// After connecting, check the locally stored pipe-request once. onSync only
// fires when the cloud copy differs from the local one, so a request that
// was interrupted (for example by an OTA reset) would otherwise be stranded.
// The grace period lets any newer data from the cloud sync first.
static const uint32_t CACHED_REQUEST_GRACE_MS = 10000;
static int64_t statusUpdatedAt = 0;

static CloudEvent telemetryEvent;
static bool publishPending = false;

struct LineReader {
    String buf;
    bool overflow = false;
};
static LineReader uartReader;
static LineReader usbReader;

// ---------------------------------------------------------------- status

static void writeStatus(bool pipeOpen)
{
    Variant d;
    d.set("state", pipeOpen ? "pipe_open" : "normal");

    if (pipeOpen) {
        Variant cur;
        cur.set("id", pending.id.c_str());
        cur.set("reason", pending.reason.c_str());
        cur.set("max_minutes", (int)cfg.pipeMaxMinutes);
        d.set("current", cur);
    }

    if (persist.last.id[0] != '\0') {
        Variant last;
        last.set("id", persist.last.id);
        last.set("reason", persist.last.reason);
        last.set("result", persist.last.result);
        last.set("detail", persist.last.detail);
        last.set("duration_s", (int)persist.last.durationS);
        d.set("last", last);
    }

    int r = statusLedger.set(d);
    statusUpdatedAt = statusLedger.lastUpdated();
    if (r != 0) {
        Log.warn("pipe-status set failed: %d", r);
    }
    else {
        Log.info("pipe-status: %s", d.toJSON().c_str());
    }
}

// ------------------------------------------------- normal mode: requests

static bool requestPipe(const String &id, const String &reason, const String &job, bool fromCloud)
{
    if (transition != Transition::NONE) {
        return false;
    }

    pending.id = id;
    pending.reason = reason;
    pending.job = job;
    pending.fromCloud = fromCloud;

    transition = Transition::WRITE_STATUS;
    Log.info("pipe requested id=%s reason=%s cloud=%d", id.c_str(), reason.c_str(), fromCloud);
    return true;
}

static void onRequestSync(Ledger ledger, void *arg)
{
    // Called on the system thread. Only set a flag; loop() does the work.
    requestSynced = true;
}

static void checkRequestLedger()
{
    LedgerData data = requestLedger.get();
    if (!data.has("id")) {
        return;
    }

    String id = data.get("id").toString();
    id.trim();
    if (id.length() == 0) {
        return;
    }

    if (id == persist.lastCloudId) {
        Log.info("pipe-request id=%s already handled, ignoring", id.c_str());
        return;
    }

    if (transition != Transition::NONE) {
        return;  // already heading into a pipe
    }

    String reason = data.has("reason") ? data.get("reason").toString() : String("cloud");
    String job = data.has("job") ? data.get("job").toJSON() : String("{}");

    if (job.length() >= sizeof(ret.job) || id.length() >= sizeof(ret.id)) {
        // Can't carry it through the reset. Report it and mark it handled so
        // it doesn't retry forever.
        Log.warn("pipe-request id=%s rejected, id or job too large", id.c_str());
        memset(&persist.last, 0, sizeof(persist.last));
        copyStr(persist.last.id, sizeof(persist.last.id), id.c_str());
        copyStr(persist.last.reason, sizeof(persist.last.reason), reason.c_str());
        copyStr(persist.last.result, sizeof(persist.last.result), "rejected");
        copyStr(persist.last.detail, sizeof(persist.last.detail), "id or job too large");
        copyStr(persist.lastCloudId, sizeof(persist.lastCloudId), id.c_str());
        savePersist();
        writeStatus(false);
        return;
    }

    requestPipe(id, reason, job, true);
}

// Normal mode: walk from "requested" to System.reset() without blocking.
static void runTransition()
{
    switch (transition) {
    case Transition::NONE:
        break;

    case Transition::WRITE_STATUS:
        // No OTA reboot in the middle of this.
        System.disableUpdates();
        writeStatus(true);
        transitionStart = millis();
        transition = Transition::WAIT_SYNC;
        break;

    case Transition::WAIT_SYNC: {
        // Make sure the cloud hears "pipe_open" before we go offline.
        bool synced = statusLedger.lastSynced() >= statusUpdatedAt && statusUpdatedAt > 0;
        bool timedOut = millis() - transitionStart > LEDGER_SYNC_TIMEOUT_MS;
        if (synced || !Particle.connected() || timedOut) {
            if (!synced) {
                Log.warn("pipe-status not confirmed synced, continuing");
            }
            // Graceful cloud disconnect (options set in setup), then radio off.
            Network.disconnect();
            Network.off();
            transitionStart = millis();
            transition = Transition::NETWORK_OFF;
        }
        break;
    }

    case Transition::NETWORK_OFF:
        if (!Network.isOn() || millis() - transitionStart > ENTER_PIPE_OFF_WAIT_MS) {
            retClear();
            ret.requested = 1;
            ret.fromCloud = pending.fromCloud ? 1 : 0;
            ret.boots = 0;
            copyStr(ret.id, sizeof(ret.id), pending.id.c_str());
            copyStr(ret.reason, sizeof(ret.reason), pending.reason.c_str());
            copyStr(ret.job, sizeof(ret.job), pending.job.c_str());
            retSave();

            Log.info("resetting into pipe mode");
            Serial1.flush();
            delay(100);
            System.reset();
        }
        break;
    }
}

// ------------------------------------------------- normal mode: telematics

static Variant typedValue(const String &s)
{
    const char *c = s.c_str();
    char *end = nullptr;

    errno = 0;
    long l = strtol(c, &end, 10);
    if (end != c && *end == '\0' && errno == 0) {
        return Variant((int)l);
    }

    end = nullptr;
    double d = strtod(c, &end);
    if (end != c && *end == '\0') {
        return Variant(d);
    }

    if (s == "true") {
        return Variant(true);
    }
    if (s == "false") {
        return Variant(false);
    }
    return Variant(c);
}

static bool parseSample(const String &line, Variant &out)
{
    int start = 0;
    size_t fields = 0;

    while (start < (int)line.length()) {
        int comma = line.indexOf(',', start);
        if (comma < 0) {
            comma = line.length();
        }

        String pair = line.substring(start, comma);
        int eq = pair.indexOf('=');
        if (eq <= 0 || ++fields > MAX_FIELDS) {
            return false;
        }

        String key = pair.substring(0, eq);
        String val = pair.substring(eq + 1);
        key.trim();
        val.trim();
        if (key.length() == 0) {
            return false;
        }

        out.set(key.c_str(), typedValue(val));
        start = comma + 1;
    }

    return fields > 0;
}

static void handleTelemetry(const String &line)
{
    if (transition != Transition::NONE) {
        say("ERR pipe");
        return;
    }
    if (publishPending) {
        say("ERR busy");
        return;
    }

    Variant sample;
    if (!parseSample(line, sample)) {
        say("ERR parse");
        return;
    }
    if (!Particle.connected()) {
        say("ERR offline");
        return;
    }
    if (!CloudEvent::canPublish(line.length())) {
        say("ERR ratelimit");
        return;
    }

    telemetryEvent.name("telemetry");
    telemetryEvent.data(sample);
    Particle.publish(telemetryEvent);
    publishPending = true;
}

static void servicePublish()
{
    if (!publishPending) {
        return;
    }
    if (telemetryEvent.isSent()) {
        say("ACK");
        telemetryEvent.clear();
        publishPending = false;
    }
    else if (!telemetryEvent.isOk()) {
        say("ERR publish %d", telemetryEvent.error());
        telemetryEvent.clear();
        publishPending = false;
    }
}

// ------------------------------------------------- modem USB mode switching
//
// The EG91's USB mode decides who can use cellular data:
//   default (0): Device OS dials normally. The host sees only serial ports
//                (its QMI interface needs qmi_wwan, which stock DEY lacks).
//   ECM (1):     the modem starts its own data session and gives the host a
//                usb0 network interface, but Device OS can no longer dial.
// So pipe mode switches to ECM, and every normal boot switches back to
// default before connecting. That also self-heals: however a pipe ends,
// the next normal boot restores default mode before going online.
//
// The setting is stored in the modem and takes effect after a power cycle.

enum class UsbPhase { WAIT_NCP, WAIT_OFF, DONE, FAILED };

struct UsbModeSwitch {
    int target = USBNET_DEFAULT;
    UsbPhase phase = UsbPhase::DONE;
    uint32_t phaseStart = 0;
    uint32_t nextTryAt = 0;        // millis() of the next query
    uint32_t readTimeoutMs = USBNET_READ_TIMEOUT_MS;
    bool stayOff = false;          // after changing the mode, leave the modem off
    uint8_t switches = 0;          // mode changes written
};
static UsbModeSwitch usbSwitch;

static int usbnetCallback(int type, const char *buf, int len, int *modeOut)
{
    if (type == TYPE_PLUS && modeOut) {
        char tmp[64];
        int n = len < (int)sizeof(tmp) - 1 ? len : (int)sizeof(tmp) - 1;
        memcpy(tmp, buf, n);
        tmp[n] = '\0';
        const char *p = strstr(tmp, "\"usbnet\",");
        if (p) {
            *modeOut = atoi(p + strlen("\"usbnet\","));
        }
    }
    return WAIT;
}

// Query the modem's USB mode. Returns RESP_OK and sets mode, or the error.
// No WITH_LOCK(Cellular): that waits for the lock with no timeout.
// Cellular.command() takes the lock itself.
static int readUsbnet(int &mode)
{
    mode = -1;
    Log.info("modem usbnet: querying");
    int r = Cellular.command(usbnetCallback, &mode, USBNET_CMD_TIMEOUT_MS, "AT+QCFG=\"usbnet\"\r\n");
    if (r != RESP_OK || mode < 0) {
        Log.info("modem usbnet: no answer (%d)", r);
        return r == RESP_OK ? RESP_ERROR : r;
    }
    return RESP_OK;
}

static bool writeUsbnet(int m)
{
    Log.info("modem usbnet: setting %d", m);
    int r = Cellular.command(USBNET_CMD_TIMEOUT_MS, "AT+QCFG=\"usbnet\",%d\r\n", m);
    if (r != RESP_OK) {
        Log.warn("modem usbnet: set failed (%d)", r);
        return false;
    }
    return true;
}

static bool timeReached(uint32_t at)
{
    return (int32_t)(millis() - at) >= 0;
}

// Start making sure the modem is in USB mode `target`.
//   firstQueryMs: wait before the first AT command (Device OS initializes the
//                 modem after it powers on; never talk to it during that).
//   stayOff:      after changing the mode, power the modem off and stop
//                 there (used right before a reset), instead of powering it
//                 back on and confirming.
static void usbModeBegin(int target, uint32_t firstQueryMs, uint32_t readTimeoutMs, bool stayOff)
{
    usbSwitch = UsbModeSwitch();
    usbSwitch.target = target;
    usbSwitch.phase = UsbPhase::WAIT_NCP;
    usbSwitch.phaseStart = millis();
    usbSwitch.nextTryAt = millis() + firstQueryMs;
    usbSwitch.readTimeoutMs = readTimeoutMs;
    usbSwitch.stayOff = stayOff;
    // Always make sure the modem is powered: the queries need it. (stayOff
    // only decides what happens after a mode change.) Harmless if it's
    // already on.
    Cellular.on();
}

// Advance the switch. Returns true once finished (DONE or FAILED).
static bool usbModeRun()
{
    switch (usbSwitch.phase) {
    case UsbPhase::DONE:
    case UsbPhase::FAILED:
        return true;

    case UsbPhase::WAIT_NCP: {
        if (!timeReached(usbSwitch.nextTryAt)) {
            return false;
        }

        int m = -1;
        int r = readUsbnet(m);

        if (r != RESP_OK) {
            if (millis() - usbSwitch.phaseStart > usbSwitch.readTimeoutMs) {
                Log.warn("modem usbnet: no answer for %lus, giving up",
                         (unsigned long)(usbSwitch.readTimeoutMs / 1000));
                usbSwitch.phase = UsbPhase::FAILED;
                return true;
            }
            // ERROR usually means Device OS is mid-setup with the modem: back
            // well off. A plain timeout: try again sooner.
            usbSwitch.nextTryAt = millis() +
                (r == RESP_ERROR ? USBNET_ERROR_BACKOFF_MS : USBNET_RETRY_GAP_MS);
            return false;
        }

        noteUsbnet(m);

        if (m == usbSwitch.target) {
            Log.info("modem usbnet=%d", m);
            usbSwitch.phase = UsbPhase::DONE;
            return true;
        }

        if (usbSwitch.switches >= USBNET_MAX_SWITCHES) {
            Log.warn("modem usbnet stuck at %d, wanted %d", m, usbSwitch.target);
            usbSwitch.phase = UsbPhase::FAILED;
            return true;
        }

        // Mark ECM before writing it, so a reset right after can't miss it.
        if (usbSwitch.target == USBNET_ECM) {
            noteUsbnet(USBNET_ECM);
        }
        if (!writeUsbnet(usbSwitch.target)) {
            usbSwitch.nextTryAt = millis() + USBNET_RETRY_GAP_MS;
            return false;
        }
        // The setting is stored in the modem now, even before its power cycle.
        noteUsbnet(usbSwitch.target);

        usbSwitch.switches++;
        Log.info("modem usbnet %d -> %d, powering modem off", m, usbSwitch.target);
        Cellular.off();
        usbSwitch.phase = UsbPhase::WAIT_OFF;
        usbSwitch.phaseStart = millis();
        return false;
    }

    case UsbPhase::WAIT_OFF:
        if (!Cellular.isOn() || millis() - usbSwitch.phaseStart > MODEM_OFF_TIMEOUT_MS) {
            if (Cellular.isOn()) {
                Log.warn("modem not confirmed off");
            }
            if (usbSwitch.stayOff) {
                Log.info("modem off, usbnet=%d on next power-up", usbSwitch.target);
                usbSwitch.phase = UsbPhase::DONE;
                return true;
            }
            Log.info("modem off, powering on");
            Cellular.on();
            usbSwitch.phase = UsbPhase::WAIT_NCP;
            usbSwitch.phaseStart = millis();
            usbSwitch.nextTryAt = millis() + USBNET_RETRY_GAP_MS;
        }
        return false;
    }

    return false;
}

// ---------------------------------------------------------------- pipe mode

static void sendReady()
{
    say("PIPE READY id=%s reason=%s max_minutes=%lu job=%s",
        ret.id, ret.reason, (unsigned long)cfg.pipeMaxMinutes, ret.job);
    lastReadyMs = millis();
}

static void resetIntoNormalMode()
{
    Log.info("resetting into normal mode");
    retClear();
    Serial1.flush();
    delay(100);
    System.reset();
}

// End the pipe: save the result, tell the host, then (in runPipe) put the
// modem back in default mode and reset. Restoring default here, while the
// modem is idle and responsive, means the next normal boot doesn't have to
// send it an AT command.
//   restore: false only when the modem was never brought up this boot.
// End the pipe: save the result, tell the host, and reset. The modem is left
// in ECM mode; the ECM marker makes the next normal boot restore default mode
// before connecting. Restoring it here doesn't work: once the host has had
// the modem's USB configured and data has flowed, the modem stops answering
// the B504e's UART until it's reinitialized. After a reset, Device OS opens a
// fresh link and the restore query answers right away.
static void finishPipe(const char *result, const char *detail)
{
    uint32_t dur = (millis() - pipeBootMs) / 1000;

    memset(&persist.last, 0, sizeof(persist.last));
    copyStr(persist.last.id, sizeof(persist.last.id), ret.id);
    copyStr(persist.last.reason, sizeof(persist.last.reason), ret.reason);
    copyStr(persist.last.result, sizeof(persist.last.result), result);
    copyStr(persist.last.detail, sizeof(persist.last.detail), detail);
    persist.last.durationS = dur;

    // Only cloud ids are remembered: an edge id must not replace the cloud id
    // still sitting in the pipe-request ledger, or that request would re-run.
    if (ret.fromCloud) {
        copyStr(persist.lastCloudId, sizeof(persist.lastCloudId), ret.id);
    }
    savePersist();

    // A reset from here on must not reopen the pipe.
    ret.closing = 1;
    retSave();

    say("PIPE CLOSING id=%s result=%s", ret.id, result);
    Log.info("pipe closed after %lus", (unsigned long)dur);
    resetIntoNormalMode();
}

static void enterPipeMode()
{
    mode = Mode::PIPE;
    pipeBootMs = millis();

    ret.boots++;
    retSave();

    say("MODE pipe id=%s reason=%s settle_s=%lu", ret.id, ret.reason,
        (unsigned long)cfg.pipeSettleS);

    if (ret.boots > MAX_PIPE_BOOTS) {
        // Repeated resets while in pipe mode (crash or watchdog). Give up
        // rather than loop offline.
        finishPipe("failed", "too many restarts in pipe mode");
        return;
    }

    // Power the modem and switch it to ECM for the host. Never connect in
    // this mode.
    pipePhase = PipePhase::PREP;
    usbModeBegin(USBNET_ECM, USBNET_FIRST_QUERY_MS, USBNET_READ_TIMEOUT_MS, false);
}

static void runPipe()
{
    switch (pipePhase) {
    case PipePhase::PREP:
        if (usbModeRun()) {
            if (usbSwitch.phase == UsbPhase::FAILED) {
                finishPipe("failed", "could not switch modem to ECM");
                return;
            }
            // Modem is in ECM. Give it time to start its data session.
            pipePhase = PipePhase::SETTLING;
            settleStartMs = millis();
        }
        break;

    case PipePhase::SETTLING:
        if (millis() - settleStartMs >= cfg.pipeSettleS * 1000) {
            pipePhase = PipePhase::OPEN;
            openedMs = millis();
            sendReady();
        }
        break;

    case PipePhase::OPEN:
        if (millis() - openedMs > cfg.pipeMaxMinutes * 60 * 1000) {
            finishPipe("timeout", "no PIPE DONE before timeout");
            return;
        }
        if (millis() - lastReadyMs >= READY_REPEAT_MS) {
            sendReady();
        }
        break;
    }
}

// ---------------------------------------------------------------- commands

// Value of key=... from the part of the line before job=/detail=, which may
// contain spaces and '=' and always runs to the end of the line.
static String getArg(const String &line, const char *key)
{
    String head = line;
    int tail = line.indexOf(" job=");
    int det = line.indexOf(" detail=");
    if (det >= 0 && (tail < 0 || det < tail)) {
        tail = det;
    }
    if (tail >= 0) {
        head = line.substring(0, tail);
    }

    String k = String(" ") + key + "=";
    int i = head.indexOf(k);
    if (i < 0) {
        return "";
    }
    int start = i + k.length();
    int end = head.indexOf(' ', start);
    return end < 0 ? head.substring(start) : head.substring(start, end);
}

static String getRest(const String &line, const char *key)
{
    String k = String(" ") + key + "=";
    int i = line.indexOf(k);
    return i < 0 ? String("") : line.substring(i + k.length());
}

static void handleStatus()
{
    say("STATUS mode=%s cloud=%s pipe_id=%s",
        mode == Mode::PIPE ? "pipe" : (transition != Transition::NONE ? "entering_pipe" : "normal"),
        Particle.connected() ? "yes" : "no",
        mode == Mode::PIPE ? ret.id : (transition != Transition::NONE ? pending.id.c_str() : ""));
}

static void handlePipeCommand(const String &line)
{
    if (line.startsWith("PIPE REQ")) {
        if (mode == Mode::PIPE) {
            // Already open. Repeat READY so a host that missed it catches up.
            if (pipePhase == PipePhase::OPEN) {
                sendReady();
            }
            else {
                say("PIPE SETTLING");
            }
            return;
        }

        String id = getArg(line, "id");
        if (id.length() == 0) {
            id = String::format("edge-%lu", (unsigned long)(Time.isValid() ? Time.now() : millis()));
        }
        String reason = getArg(line, "reason");
        if (reason.length() == 0) {
            reason = "edge";
        }
        String job = getRest(line, "job");
        if (job.length() == 0) {
            job = "{}";
        }

        if (job.length() >= sizeof(ret.job) || id.length() >= sizeof(ret.id)) {
            say("ERR too_long");
        }
        else if (requestPipe(id, reason, job, false)) {
            say("PIPE ACCEPTED id=%s", id.c_str());
        }
        else {
            say("PIPE BUSY");
        }
    }
    else if (line.startsWith("PIPE DONE")) {
        if (mode != Mode::PIPE) {
            say("ERR no_pipe");
            return;
        }
        String result = getArg(line, "result");
        if (result.length() == 0) {
            result = "ok";
        }
        // The host put the modem back in default mode over USB (and rebooted
        // it), so the next boot doesn't need to touch it: clear the ECM
        // marker. Without this, the next boot restores the modem itself.
        if (getArg(line, "modem") == "restored") {
            noteUsbnet(USBNET_DEFAULT);
            Log.info("host restored modem default mode");
        }
        finishPipe(result.c_str(), getRest(line, "detail").c_str());
    }
    else {
        say("ERR unknown");
    }
}

static void handleLine(String line)
{
    line.trim();
    if (line.length() == 0) {
        return;
    }

    if (line.startsWith("PIPE ")) {
        handlePipeCommand(line);
    }
    else if (line == "STATUS") {
        handleStatus();
    }
    else if (mode == Mode::PIPE) {
        say("ERR pipe");  // offline, host should hold telemetry
    }
    else {
        handleTelemetry(line);
    }
}

static void pollPort(Stream &port, LineReader &r)
{
    while (port.available() > 0) {
        char c = (char)port.read();
        if (c == '\n' || c == '\r') {
            if (r.overflow) {
                say("ERR too_long");
            }
            else if (r.buf.length() > 0) {
                handleLine(r.buf);
            }
            r.buf = "";
            r.overflow = false;
        }
        else if (r.buf.length() < MAX_LINE) {
            r.buf += c;
        }
        else {
            r.overflow = true;
        }
    }
}

// ---------------------------------------------------------------- main

void setup()
{
    Watchdog.init(WatchdogConfiguration().timeout(WATCHDOG_TIMEOUT));
    Watchdog.start();

    Particle.setDisconnectOptions(CloudDisconnectOptions().graceful(true).timeout(5s));

    loadConfig();
    Serial.begin();
    Serial1.begin(cfg.uartBaud);
    loadPersist();

    if (retValid() && ret.requested) {
        if (ret.closing) {
            // Reset while closing a finished pipe: don't reopen it. Its
            // result is already saved; the ECM marker handles the modem.
            Log.info("pipe %s already closed, continuing in normal mode", ret.id);
        }
        else {
            enterPipeMode();
            return;
        }
    }

    // Normal mode.
    retClear();

    requestLedger = Particle.ledger(REQUEST_LEDGER);
    requestLedger.onSync(onRequestSync);
    statusLedger = Particle.ledger(STATUS_LEDGER);

    say("MODE normal");

    lastOnlineMs = millis();

    if (ecmMarker == ECM_MARKER) {
        // After a pipe the modem is still in ECM mode, which would block
        // Device OS from dialing. Restore default mode first (~5 s in, when
        // the modem answers right away after a reset).
        Log.info("modem may be in ECM mode after a pipe, restoring default before connecting");
        normalPrep = true;
        // stayOff: after switching, power the modem off and reset (below),
        // so the connection always starts from a clean boot.
        usbModeBegin(USBNET_DEFAULT, USBNET_FIRST_QUERY_MS, USBNET_READ_TIMEOUT_MS, true);
        return;
    }

    // Connect exactly like a stock device: no AT commands.
    Particle.connect();
}

void loop()
{
    Watchdog.refresh();

    pollPort(Serial1, uartReader);
    pollPort(Serial, usbReader);  // bench testing from a laptop

    if (mode == Mode::PIPE) {
        runPipe();
        return;
    }

    if (normalPrep) {
        if (usbModeRun()) {
            normalPrep = false;
            if (usbSwitch.phase == UsbPhase::DONE && usbSwitch.switches > 0) {
                // Switched the modem's mode mid-boot. Connecting straight
                // after that gave stalled handshakes and an unhealthy session
                // in testing, so reset and connect from a clean boot instead.
                // The ECM marker is already cleared.
                Log.info("modem restored to default mode, resetting for a clean boot");
                resetIntoNormalMode();
                return;
            }
            if (usbSwitch.phase == UsbPhase::FAILED) {
                Log.warn("could not confirm modem default mode, connecting anyway");
            }
            Particle.connect();
            lastOnlineMs = millis();
        }
        return;
    }

    // Fallback: still not connected after FALLBACK_CHECK_MS. If a pipe was
    // cut short by power loss, the ECM marker is gone too, so check the
    // modem's mode directly.
    if (fallbackActive) {
        if (usbModeRun()) {
            fallbackActive = false;
            if (usbSwitch.phase == UsbPhase::DONE && usbSwitch.switches > 0) {
                // The modem was in ECM. It's now set to default and powered
                // off: reset, so the next boot connects like a stock device.
                Log.info("fallback: modem restored to default mode, resetting");
                resetIntoNormalMode();
                return;
            }
            Log.info("fallback check done (%s), reconnecting",
                     usbSwitch.phase == UsbPhase::DONE ? "modem already in default mode" : "no answer");
            Particle.connect();
            lastOnlineMs = millis();
        }
    }
    else if (!fallbackStarted && !Particle.connected() && !Cellular.ready() &&
             transition == Transition::NONE && millis() - lastOnlineMs > FALLBACK_CHECK_MS) {
        // Only when the cellular link itself is down: in ECM mode Device OS
        // can't bring it up at all. If the link is up but the cloud isn't
        // (a slow handshake), the modem is fine; leave Device OS to finish.
        fallbackStarted = true;
        fallbackActive = true;
        Log.warn("not connected after %lu min, checking modem USB mode",
                 (unsigned long)(FALLBACK_CHECK_MS / 60000));
        // Pause Device OS's connection attempts first, so the modem isn't busy
        // with a failing dial and answers the query right away. After a mode
        // change: power off and reset (stayOff). Otherwise: reconnect.
        Network.disconnect();
        usbModeBegin(USBNET_DEFAULT, USBNET_RETRY_GAP_MS, USBNET_READ_TIMEOUT_MS, true);
    }

    // Reset if the cloud stays unreachable for too long. A pipe transition
    // goes offline on purpose and resets on its own, so it's excluded.
    if (Particle.connected()) {
        lastOnlineMs = millis();
    }
    else if (transition == Transition::NONE && millis() - lastOnlineMs > OFFLINE_RESET_MS) {
        Log.warn("offline for %lu min, powering modem off and resetting",
                 (unsigned long)(OFFLINE_RESET_MS / 60000));
        Cellular.off();
        delay(RESET_OFF_WAIT_MS);
        System.reset();
    }

    servicePublish();

    // Report the current state (and the last pipe's result) once per boot.
    if (!statusReported && Particle.connected()) {
        statusReported = true;
        connectedAt = millis();
        writeStatus(false);
    }

    // Check the local copy of pipe-request once per boot, after a grace
    // period so fresher cloud data (via onSync) wins over a stale cache.
    if (!cachedRequestChecked && connectedAt != 0 && Particle.connected() &&
        millis() - connectedAt >= CACHED_REQUEST_GRACE_MS) {
        cachedRequestChecked = true;
        requestSynced = true;
    }

    // Act on pipe-request after a sync, or the one-time check above.
    if (requestSynced) {
        requestSynced = false;
        checkRequestLedger();
    }

    if (transition == Transition::NONE) {
        // Let a pending OTA (firmware or environment) apply at a safe moment.
        if (System.updatesPending()) {
            System.enableUpdates();
        }
    }
    else {
        runTransition();
    }
}