# Protocol and configuration reference

This is the interface between the B504e firmware, the host, and the cloud. A host implementation only needs this document; `host/pipe_client.sh` is a working example.

## UART protocol

Serial1 on the B504e (TX on D9, RX on D10), 115200 baud by default, 8N1, no flow control. Plain text, one message per line, newline-terminated. The B504e ends its lines with `\r\n`; the host may send `\n` or `\r\n`.

Every line the B504e sends to the host is also mirrored to its USB serial port, and commands are accepted on USB serial too. That's useful for bench testing from a laptop.

### Host to B504e

| Message | When | Reply |
|---|---|---|
| `key=value,key=value,...` | Normal mode | `ACK` or `ERR <reason>` |
| `PIPE REQ [reason=<text>] [id=<id>] [job=<rest of line>]` | Any time | `PIPE ACCEPTED id=<id>` or `PIPE BUSY`. In pipe mode: a repeat of `PIPE READY`, or `PIPE SETTLING` |
| `PIPE DONE [result=<text>] [modem=restored] [detail=<rest of line>]` | Pipe mode | `PIPE CLOSING id=<id> result=<result>`, then the B504e resets. Outside pipe mode: `ERR no_pipe` |
| `STATUS` | Any time | `STATUS mode=<normal\|entering_pipe\|pipe> cloud=<yes\|no> pipe_id=<id>` |

Notes:

- `job=` and `detail=` run to the end of the line and may contain spaces, so they must come last.
- `PIPE REQ` without an `id` gets one generated (`edge-<unix time>`). `reason` defaults to `edge`.
- `PIPE DONE` without a `result` is treated as `result=ok`.
- `modem=restored` tells the B504e the host already put the modem back in its default USB mode and rebooted it (see host responsibilities). The B504e's next boot then doesn't touch the modem. Without it, the next boot restores the modem itself, which takes about a minute longer. Anything in `result` and `detail` is passed through to the `pipe-status` Ledger, so the host can report details like throughput (`detail=200MB in 290s 5.8Mbps`).
- Lines longer than 600 characters are rejected with `ERR too_long`.

### Telemetry

Each `key=value,...` line becomes one cloud event named `telemetry`, with structured data. Values are typed automatically: integers, decimals, `true`/`false`, otherwise strings. For example:

```
rpm=2400,fuel=68.5,engine=true,state=docked
```

is published as `{"rpm":2400,"fuel":68.5,"engine":true,"state":"docked"}`. Up to 32 fields per line.

The B504e publishes one sample at a time and replies when the cloud acknowledges it.

### B504e to host

| Message | Meaning |
|---|---|
| `ACK` | Telemetry sample published |
| `ERR busy` | A publish is still in progress. Retry shortly. |
| `ERR offline` | Not connected to the cloud. Retry later. |
| `ERR ratelimit` | Publish rate limit reached. Retry shortly. |
| `ERR pipe` | A pipe is open or opening. Hold telemetry until `MODE normal`. |
| `ERR parse` | The line isn't valid `key=value` pairs. Don't retry. |
| `ERR too_long` | The line is too long. Don't retry. |
| `ERR publish <code>` | The cloud publish failed. |
| `ERR no_pipe` | `PIPE DONE` received outside pipe mode. |
| `ERR unknown` | Unrecognized `PIPE` command. |
| `MODE normal` | Sent at every normal boot. After a pipe, it means the pipe is closed. It does **not** mean the B504e is online yet: that takes about 35 more seconds. |
| `MODE pipe id=<id> reason=<reason> settle_s=<n>` | Sent at every pipe boot. The modem is being prepared; wait for `PIPE READY`. |
| `PIPE ACCEPTED id=<id>` | The B504e is going offline. Expect a reset, then `MODE pipe`. |
| `PIPE BUSY` | A pipe transition is already underway. |
| `PIPE SETTLING` | Pipe mode, but the modem isn't ready for the host yet. |
| `PIPE READY id=<id> reason=<reason> max_minutes=<n> job=<json>` | The modem is the host's to use. Repeated every 30 s while the pipe is open; treat repeats as idempotent. |
| `PIPE CLOSING id=<id> result=<result>` | The pipe is over and the B504e is resetting. If the host sent `modem=restored`, the next boot is an ordinary one; otherwise it first restores the modem's default mode. |

### Example sequences

Edge-initiated (the host asks for the pipe):

```
host  -> PIPE REQ reason=button
b504e -> PIPE ACCEPTED id=edge-1794518259
         (reports pipe_open, disconnects, resets)
b504e -> MODE pipe id=edge-1794518259 reason=button settle_s=60
         (switches the modem to ECM, settles)
b504e -> PIPE READY id=edge-1794518259 reason=button max_minutes=20 job={}
         (host downloads over usb0)
         (host, over the modem's USB AT port:
          AT+QCFG="usbnet",0 -> OK, then AT+CFUN=1,1; the modem reboots)
host  -> PIPE DONE result=ok modem=restored detail=200MB in 290s 5.8Mbps
b504e -> PIPE CLOSING id=edge-1794518259 result=ok
         (resets)
b504e -> MODE normal
         (ordinary boot: connects like a stock device)
         (switches the modem back, reconnects, reports the result)
```

Cloud-initiated: the same, except it starts with a new `id` in the `pipe-request` Ledger instead of `PIPE REQ`, and `PIPE READY` carries that request's `id`, `reason`, and `job`.

## Host responsibilities

1. **Use the modem's network interface only between `PIPE READY` and `PIPE DONE`.** Outside a pipe, the modem belongs to Device OS. Since the modem is only in ECM mode during pipes, `usb0` only exists during pipes, so running DHCP whenever `usb0` appears (the provided udev rule and service) gives the host a route exactly when it should have one.
2. **Keep the modem's USB deauthorized outside pipes, and never run ModemManager or anything else against its ports.** While a host has the modem's USB configured, even with no program using it, the modem stops answering Device OS reliably: on the bench, the B504e lost contact with the modem 2 to 4 minutes into every session, and vitals requests went unanswered. With the device deauthorized (`echo 0 > /sys/bus/usb/devices/<modem>/authorized`), it ran normally. So the host authorizes the modem's USB at `PIPE READY` and never deauthorizes it mid-flight: at the end of the pipe the modem reboots (step 3 below), and the udev rule deauthorizes it when it re-enumerates. Changing the modem's USB state while the modem is active stops it answering the B504e on the UART.
3. **When the download is done, restore the modem before sending `PIPE DONE`.** Over the modem's USB AT port (`/dev/ttyUSB2` on the CC93), send `AT+QCFG="usbnet",0`, wait for `OK`, then send `AT+CFUN=1,1` (the modem reboots and drops off USB; don't wait for a reply). Then send `PIPE DONE ... modem=restored`. Only the host can do this: after the host has used the modem's USB, the modem stops answering the B504e's UART until it restarts. If the first command fails, send `PIPE DONE` without `modem=restored`, and the B504e restores the modem on its next boot instead.
4. **Hold telemetry while a pipe is open or opening** (`ERR pipe`, or after `PIPE ACCEPTED`), and resume after `MODE normal`.
5. **Treat `PIPE READY` as idempotent.** It repeats every 30 s, and repeats queue up while the host is busy downloading. But after a `MODE pipe` line, treat the next `PIPE READY` as new, even with the same `id`: the B504e restarted pipe mode (for example after a reset) and is reopening that pipe.
6. **Before reporting a failure, check for a B504e restart.** If a download fails, read any lines waiting on the UART first. A `MODE pipe` among them means the B504e reset and is reopening the pipe: don't send `PIPE DONE`, just serve the next `PIPE READY`. Otherwise, report `result=failed`, and request a new pipe to retry. Resumable downloads (HTTP range requests, `wget -c`) avoid re-downloading what already arrived.
7. **Otherwise, don't reset or reconfigure the modem** (radio state, PDP contexts, USB mode). The end-of-pipe restore in step 3 is the only exception.
8. **Verify downloaded files** with a hash or signature. BusyBox `wget` doesn't strictly verify TLS certificates.

## Ledgers

Create both Ledger definitions in the Particle console before flashing.

| Name | Type | Scope | Purpose |
|---|---|---|---|
| `pipe-request` | Cloud to device | Device | Request a pipe |
| `pipe-status` | Device to cloud | Device | Current and last pipe state |

### `pipe-request`

**Setup:** create the device's instance in the console with these values (Default view, two key/value rows):

| Key | Value |
|---|---|
| `id` | `test-001` |
| `reason` | `bench-test` |

Creating it opens the first pipe as soon as the B504e syncs, so have the host ready first. After that, change `id` to a new value for each pipe.

**Full format:**

```json
{
  "id": "update-2026-11-12-001",
  "reason": "system-update",
  "job": { "url": "https://example.com/update.swu", "sha256": "..." }
}
```

- `id` (required): unique per request. Each `id` opens at most one pipe. Once that pipe finishes, with any result, the same `id` is ignored, so use a new `id` to retry.
- `reason` (optional): free text. Defaults to `cloud`.
- `job` (optional): anything the host needs, such as a download URL and hash. The B504e passes it to the host as compact JSON in `PIPE READY` without interpreting it. Up to 511 bytes as JSON.

In the console's Default view, `id` and `reason` can be entered as key/value rows. `job` is an object, so it needs the Advanced (JSON) view.

The B504e checks `pipe-request` whenever the Ledger syncs from the cloud, and also checks its local copy once, 10 seconds after each connection. The second check catches a request that was interrupted (for example by an OTA reset) and wouldn't otherwise sync again. The 10-second delay lets newer cloud data arrive first.

The last handled cloud `id` is stored in EEPROM, so the same request never runs twice, even across power loss. Host-initiated pipes don't change it.

Setting the Ledger through the API:

```
curl -X PUT "https://api.particle.io/v1/orgs/<org>/ledgers/pipe-request/instances/<device_id>" \
  -H "Authorization: Bearer <token>" \
  -H "Content-Type: application/json" \
  -d '{"instance":{"data":{"id":"test-001","reason":"cloud","job":{}}}}'
```

### `pipe-status`

```json
{
  "state": "pipe_open",
  "current": { "id": "test-005", "reason": "bench-test", "max_minutes": 20 },
  "last": { "id": "test-004", "reason": "bench-test", "result": "ok", "detail": "100MB in 144s 5.8Mbps", "duration_s": 234 }
}
```

- `state`: `normal` or `pipe_open`. It's written and confirmed synced before the B504e goes offline, so `pipe_open` stays visible in the cloud for the whole pipe.
- `current`: present only while a pipe is open.
- `last`: the most recent completed pipe, reported after the B504e reconnects. `duration_s` covers the whole pipe boot, including modem preparation. `result` is what the host sent in `PIPE DONE`, or one of:
  - `timeout`: no `PIPE DONE` within `PIPE_MAX_MINUTES` of `PIPE READY`
  - `failed`, detail `could not switch modem to ECM`
  - `failed`, detail `too many restarts in pipe mode`: the B504e reset three times during one pipe
  - `rejected`, detail `id or job too large`

If `state` stays `pipe_open` much longer than `max_minutes` plus about 3 minutes of overhead, something went wrong.

## Environment variables

Create these in the product with the values below. They're read once at boot, require Device OS 6.4.0 or later, and can be overridden per device.

| Name | Value to set | Range | Purpose |
|---|---|---|---|
| `PIPE_MAX_MINUTES` | `20` | 1 to 240 | How long the pipe stays open after `PIPE READY` without `PIPE DONE` |
| `PIPE_SETTLE_S` | `60` | 0 to 600 | Wait after the modem is confirmed in ECM mode, before `PIPE READY` |
| `UART_BAUD` | `115200` | 9600 to 1000000 | Serial1 baud rate. The host must match. Above 115200, consider RTS/CTS flow control (not implemented). |

These values are also the firmware's built-in defaults, used if a variable is missing or out of range, so a device behaves the same before its first environment update arrives.

Environment variable changes are delivered as an OTA update. If one is pending, the firmware applies it automatically whenever no pipe transition is underway.

## Timing

| Phase | Typical |
|---|---|
| `PIPE ACCEPTED` to `MODE pipe` | ~15 s (status sync, graceful disconnect, reset) |
| `MODE pipe` to `PIPE READY` | ~90 s (modem switch to ECM ~30 s, plus `PIPE_SETTLE_S`) |
| `PIPE CLOSING` to `MODE normal` | ~1 s |
| `MODE normal` to cloud connected | ~25 s with `modem=restored` (an ordinary boot). If the B504e has to restore the modem itself, it does so, resets, and connects from a clean boot: ~75 s after a timeout (measured) |
| Cold boot to cloud connected | ~20 s (up to ~1 minute if Device OS has to recover the modem) |

A B504e reset drops the modem off the host's USB bus. On a cold boot, Device OS may power-cycle the modem a second time during initialization, about 50 seconds in.

## Firmware safeguards

| Situation | Behavior |
|---|---|
| Reset during a pipe (crash, watchdog, reset button) | The pipe request is in retained memory, so the B504e boots back into pipe mode and reopens the pipe for the same request. |
| Reset while closing a pipe | A retained "closing" flag sends the next boot to normal mode instead of reopening the finished pipe. |
| Pipe ended without the host restoring the modem | A retained marker tracks whether the modem may be in ECM mode. The next normal boot sees it, restores default mode, and resets, so it connects from a clean boot. |
| Power loss during a pipe | Retained memory is lost, so the B504e boots into normal mode. If it hasn't connected after 3 minutes and the cellular link itself is down (as it is in ECM mode), it pauses its connection attempts and checks the modem's mode. If the modem was left in ECM, it restores default mode, powers the modem off, and resets; the next boot connects normally. |
| Host never sends `PIPE DONE` | Times out after `PIPE_MAX_MINUTES`, reports `result=timeout`, returns to normal. |
| Repeated resets during a pipe | After 3 unexpected resets in pipe mode for the same request, gives up with `result=failed` and returns to normal. |
| Firmware hang | A 120-second hardware watchdog resets the B504e. |
| `pipe-status` can't be confirmed synced | Continues after 35 seconds; the Ledger syncs on the next connection. |
| OTA update pending | Applied only when no pipe transition is underway. |
| Modem not answering AT commands | Normal for up to ~55 s after a B504e reset, while Device OS recovers the modem. The firmware queries briefly every 10 s so it doesn't get in the way. After 3 minutes without an answer, a pipe boot ends with `result=failed`, and a normal boot tries to connect anyway (the offline reset below covers it if that fails). |
| Cloud unreachable in normal mode | After 10 minutes offline, the B504e resets. |
