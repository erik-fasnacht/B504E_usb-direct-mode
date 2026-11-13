# Test guide

Test scenarios for USB Direct Mode, with steps and expected results. Each one assumes the setup from [BRINGUP.md](BRINGUP.md) is complete: the B504e flashed and online, the CC93 configured, and the UART verified in both directions.

For each test, keep two windows open: the CC93 console running `~/pipe_client.sh`, and `particle serial monitor --follow` for the B504e. Wait for the B504e to return to breathing cyan before starting the next test.

## Data usage

| Test | Data |
|---|---|
| One pipe with default settings | ~100 MB |
| One pipe with `CHUNKS=4` | ~200 MB |
| Timeout test (no download) | ~0 MB |

## Tests

### T1. Normal boot

**Steps:** reset the B504e.

**Expected (B504e log):**

```
[system] INFO: Cloud connected
[app] INFO: pipe-status: {"state":"normal",...}
```

LED: breathing cyan. On the CC93, `ip addr show usb0` reports the device doesn't exist.

**Pass:** connected within about a minute, with no `usb0` on the CC93.

### T2. Cloud-triggered pipe (100 MB)

**Steps:** with `~/pipe_client.sh` running, set `pipe-request` to a new `id` (for example `test-101`).

**Expected (CC93):** `MODE pipe`, then `PIPE READY` about 90 seconds later, `modem USB authorized`, `usb0 up`, `clock synced`, two chunks, `modem set to default USB mode`, `modem rebooting`, `>> PIPE DONE result=ok modem=restored detail=100MB in ...`, `PIPE CLOSING`, `MODE normal`.

**Expected (B504e):** `pipe requested id=test-101 ... cloud=1`, the graceful disconnect, `resetting into pipe mode`, then `modem usbnet 0 -> 1, powering modem off` and `modem usbnet=1`. After `PIPE DONE`: `host restored modem default mode`, `resetting into normal mode`, then an ordinary boot with no `modem usbnet` lines and `Cloud connected` about 25 seconds later.

**Pass:** `pipe-status` shows `state: normal` and `last.id: test-101`, `last.result: ok`, and `usb0` is gone from the CC93 afterwards.

### T3. Edge-triggered pipe (button)

**Steps:** with `~/pipe_client.sh` running, press **SW5**.

**Expected (CC93):**

```
button pressed
>> PIPE REQ reason=button
<< PIPE ACCEPTED id=edge-...
<< MODE pipe id=edge-... reason=button settle_s=60
<< PIPE READY id=edge-...
```

then the download and the same closing sequence as T2.

**Pass:** `pipe-status` shows `last.reason: button`, `last.id: edge-...`, `last.result: ok`.

### T4. 200 MB

**Steps:** start the client with `CHUNKS=4 ~/pipe_client.sh`, then run T2 or T3.

**Pass:** `PIPE DONE result=ok detail=200MB in ...`, and the B504e reconnects.

### T5. Duplicate request protection

**Steps:** after T2 completes, reset the B504e without changing `pipe-request`.

**Expected (B504e):** about 10 seconds after `Cloud connected`:

```
[app] INFO: pipe-request id=test-101 already handled, ignoring
```

**Pass:** no new pipe opens. Also after an edge pipe (T3): the stored cloud `id` must still be ignored on the next boot.

### T6. Host reboot

**Steps:** in normal mode, reboot the CC93 (`reboot`).

**Pass:** the B504e log shows nothing new and stays breathing cyan. Rebooting the host doesn't affect Device OS.

### T7. B504e reset in normal mode

**Steps:** press reset on the breakout.

**Pass:** the modem drops off the CC93's USB bus and returns (watch with `journalctl -k -f` on the CC93), deauthorized again, and the B504e reconnects. No `usb0` or `ttyUSB` devices appear on the CC93.

### T8. Timeout

**Steps:** set the `PIPE_MAX_MINUTES` environment variable to `2` and apply it. Trigger a pipe, and once `PIPE READY` appears, stop the client with Ctrl-C so no `PIPE DONE` is sent.

**Expected (B504e):** about 2 minutes after `PIPE READY`:

```
PIPE CLOSING id=... result=timeout
```

then the normal return sequence.

**Pass:** `pipe-status` shows `last.result: timeout`, and the B504e is back online. Set `PIPE_MAX_MINUTES` back afterwards.

### T9. Telemetry relay

**Steps:** in normal mode, on the CC93:

```
stty -F /dev/ttyXBee 115200 raw -echo
(timeout 5 cat /dev/ttyXBee &) ; sleep 0.5 ; printf 'rpm=2400,fuel=68.5,engine=true\n' > /dev/ttyXBee ; sleep 5
```

**Pass:** `ACK` on the CC93, and a `telemetry` event with `{"rpm":2400,"fuel":68.5,"engine":true}` in the console's event stream. During a pipe, the same line gets `ERR pipe`.

### T10. Power loss during a pipe

**Steps:** during a pipe's download, remove power from the breakout for 10 seconds, then restore it.

**Pass:** the B504e boots into normal mode and can't connect at first (modem still in ECM). About 3 minutes in, it logs `not connected after 3 min, checking modem USB mode`, then `fallback: modem restored to default mode, resetting`, and the next boot connects normally. For a cloud-triggered pipe, the request runs once more, since it never completed. The CC93's download fails, and the client reports nothing for that pipe.

### T11. Reset during a pipe

**Steps:** once `PIPE READY` appears, press reset on the breakout. Then, on a second run, press reset right after `PIPE CLOSING`.

**Pass, reset during the pipe:** the B504e boots back into pipe mode (`MODE pipe` with the same `id`) and sends `PIPE READY` again. If the reset lands mid-download, the client's download fails once the modem drops off USB; the client then logs `B504e restarted pipe mode, waiting for it to reopen the pipe` instead of reporting a failure. Either way, it serves the reopened pipe from the start, and it closes with `result=ok`.

**Pass, reset while closing:** the B504e boots into normal mode (`pipe ... already closed, continuing in normal mode`), restores the modem's default mode if needed, and connects. No pipe reopens.

### T12. Power-up order

**Steps:** from fully off (both barrel connectors unplugged, all cables connected, 30 s), power up in each order: (A) breakout first, then the CC93 once the B504e is breathing cyan; (B) the CC93 first, then the breakout once the CC93 is at its login prompt; (C) both at the same moment. For each, check that the B504e connects, the modem's USB is deauthorized (`authorized` = `0`), vitals work, and a 1 MB pipe completes with `modem=restored`, with vitals working afterwards.

**Pass:** all checks in every order.

## Recorded results

Bench setup: B504e on Device OS 6.4.1, ConnectCore 93 DVK on DEY 5.0-r3 (stock), KORE Super SIM roaming on Verizon LTE. All results are on the final firmware, client script, and udev rules unless noted.

| Test | Result | Notes |
|---|---|---|
| T1 Normal boot | Pass | Cold start (both boards powered together): connected at 19.3 s with no modem commands from the firmware. Warm restart (environment OTA): 3.9 s. |
| T2 Cloud-triggered pipe | Pass | Closes with `modem=restored`; back online ~25 s after `PIPE CLOSING`. |
| T3 Edge-triggered pipe | Pass | Same as T2. 1 MB pipes repeated several times: ~2:20 from button press to back online, consistently. |
| T4 200 MB | Pass | Edge-triggered: 200 MB in 218 s (7.7 Mbps), back online 23.7 s after the reset, next vitals request handled normally. Cloud-triggered (earlier firmware): 200 MB in 290 s (5.8 Mbps). |
| T5 Duplicate protection | Pass | Handled cloud `id`s are ignored on every later boot, including after edge pipes. |
| T6 Host reboot | Pass | No effect on Device OS. The CC93 booting during a B504e session (deauthorize rule active) was followed by 7 vitals requests over 11 minutes without a modem error. |
| T7 B504e reset | Pass | Including an OTA-triggered restart with the host running: reconnected in 3.9 s. |
| T8 Timeout | Pass | `PIPE_MAX_MINUTES` = 5, no client running: `result=timeout` exactly 300 s after `PIPE READY`, `id` marked handled. The next boot restored the modem, reset, and connected from a clean boot; back online ~75 s after the timeout, session healthy. |
| T9 Telemetry relay | Pass | `rpm=2400,fuel=68.5,engine=true` was published as `telemetry` with typed data: `{"engine":true,"fuel":68.5,"rpm":2400}`. |
| T10 Power loss during a pipe | Pass | Power removed right after the host brought up `usb0`. The normal boot couldn't dial (modem left in ECM); at 180 s the fallback paused the connection, restored default mode (query answered in 25 ms), and reset. Connected 70 s into the next boot, ~4 min 20 s from power-up, no manual steps. |
| T11 Reset during a pipe | Pass | Breakout reset during a 200 MB download: the B504e reopened the pipe for the same `id`, the client detected the restart and served it again, and it closed with `modem=restored`. |
| T12 Power-up order | Pass | Breakout first, CC93 first, and both together all passed every check. |

## Findings

These shaped the design and are worth knowing for anyone building on it.

- **ECM mode blocks Device OS.** With the modem in ECM mode, Device OS can't dial (`Failed to enter data mode`). Hence switching to ECM only for pipes.
- **A host with the modem's USB configured disrupts Device OS.** While the CC93 has the modem's USB configured, even with no program using its ports, each vitals request is followed 94 seconds later by `No response from NCP` and a ~45 s outage. Deauthorizing the modem's USB outside pipes fixes it (the udev rule). Changing the modem's URC port didn't help, so the modem's configuration is left at its defaults.
- **After the host has used the modem's USB, the modem stops answering the B504e's UART** until the modem restarts. So the B504e can't restore the modem at the end of a pipe; the host restores it over USB and reboots it (`modem=restored`). Without that, recovery takes ~95 s (Device OS recovers the modem, then the firmware restores it), and in one case over 6 minutes.
- **AT commands during Device OS's modem startup derail it.** Normal boots send the modem nothing, unless a pipe ended uncleanly.
- **Connecting right after switching the modem's mode mid-boot is unreliable** (two cloud handshakes stalled for exactly 180 s). The firmware resets after any mid-boot mode switch, so connections always start from a clean boot.
- **The modem never reports powered off on this bench setup,** and Device OS sometimes logs `Failed to power off modem`. The firmware doesn't rely on the modem powering off.
- **A B504e reset doesn't reset the modem.** It's left in multiplexer mode, and Device OS takes up to ~55 s to recover it (`No response from NCP`). Allow a minute after a reset button press.
- **Power-up order no longer matters** with the deauthorize rule (T12), but before it, powering the CC93 with the breakout off was once followed by an unresponsive modem. For a product, keep the modem's USB 5 V and the UART from being live while the SoM is unpowered, or power both from the same rail.

## Known limitations

- **Per-pipe overhead is about 2 minutes:** ~16 s to enter pipe mode, ~90 s to switch the modem to ECM and settle, ~25 s back online. `PIPE_SETTLE_S` (60 s) is conservative and could likely be lowered.
- **Entering a pipe waits up to 15 s** after turning the network off.
- **Telemetry is one publish at a time** (`ERR busy` for anything sent meanwhile), every reading goes to the `telemetry` event, and the host holds telemetry during pipes.
- **The host must not run ModemManager.**
- **Every B504e reset drops the modem off the host's USB bus,** with a new MAC address and DHCP lease each time.
- **BusyBox `wget` doesn't strictly verify TLS certificates.** Real update downloads should be checked with a hash or signature.
- **Test downloads use Cloudflare's speed test endpoint,** which refuses single requests of 100 MB or more (hence 50 MB chunks). Some standalone speed test servers weren't reachable over this carrier.
- **Not yet verified:** the hardware watchdog (120 s) actually firing, and UART baud rates above 115200. The crash-loop guard fired once during development.
