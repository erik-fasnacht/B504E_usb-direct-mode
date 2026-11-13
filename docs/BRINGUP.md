# Bring-up guide

This guide takes you from boxed hardware to working USB Direct Mode: the B504e online in normal mode, and the ConnectCore 93 (CC93) downloading over the B504e's modem via USB on request, from either the cloud or a button on the CC93. It uses a Mac with Apple Silicon as the workstation.

Tested with Digi Embedded Yocto (DEY) 5.0-r3 on the CC93 and Device OS 6.4.1 on the B504e.

## Contents

1. [Prerequisites](#1-prerequisites)
2. [Hardware](#2-hardware)
3. [Wiring](#3-wiring)
4. [Set up the Mac](#4-set-up-the-mac)
5. [Serial consoles](#5-serial-consoles)
6. [Configure the CC93](#6-configure-the-cc93)
7. [Set up the cloud](#7-set-up-the-cloud)
8. [Flash the B504e](#8-flash-the-b504e)
9. [Verify](#9-verify)
10. [Run a pipe](#10-run-a-pipe)
11. [Troubleshooting](#11-troubleshooting)
12. [Manual modem reset](#12-manual-modem-reset)

## 1. Prerequisites

**The CC93 must already be running Digi Embedded Yocto 5.0.** Installing DEY isn't covered here; follow Digi's ConnectCore 93 documentation for that. Any DEY 5.0 image works as long as it provides:

- The `cdc_ether` kernel driver (`CONFIG_USB_NET_CDCETHER`, a module in stock DEY 5.0)
- systemd and udev
- BusyBox `sh`, `stty`, `wget`, `awk`, `udhcpc`, and `microcom`, plus the `ip` command
- libgpiod tools (`gpiomon`), for the button

Check the driver on the CC93:

```
zcat /proc/config.gz | grep CONFIG_USB_NET_CDCETHER
```

It should print `CONFIG_USB_NET_CDCETHER=m` (or `=y`).

**On the Particle side,** you need a Particle account, and the B504e should be in a product (environment variables are set through a product).

## 2. Hardware

| Item | Notes |
|---|---|
| Mac with Apple Silicon | Admin (sudo) access |
| Digi ConnectCore 93 Development Kit | Running DEY 5.0, with its power supply |
| Particle B504e | Quectel EG91 LTE Cat 1 modem |
| Particle M.2 SoM breakout board | Powered from its **own supply** (VIN barrel or battery), not USB alone. The modem needs more power than a USB port provides. |
| Cellular antenna | Attached to the breakout |
| SIM with an active data plan | Each 200 MB test uses 200 MB of data |
| USB-C to USB-C (or A) cable | Mac to the breakout's **MCU** USB-C port |
| USB cable for the CC93 console | DVK console port to the Mac |
| USB-C to USB-A cable, **data capable** | Breakout's **cellular modem** USB-C port to the DVK |
| 3 jumper wires | B504e UART to the DVK's XBee socket. The socket is 2 mm pitch; an XBee-to-0.1" adapter makes this much more reliable. |

> **Cables matter.** Many USB cables bundled with chargers and accessories are charge-only. If the modem doesn't show up on the CC93, try another cable first. Label the one that works.

**Breakout USB-C ports:**

- **MCU USB-C:** the B504e's processor, for flashing and logs. Goes to the Mac.
- **Cellular modem USB-C:** connects directly to the EG91. Goes to the CC93.

**CC93 DVK ports:**

- **Console USB:** serial console. Goes to the Mac.
- **Stacked USB-A pair:** USB host ports. The modem goes into either one.
- **XBee socket:** UART4 of the i.MX93, `/dev/ttyXBee` in Linux. The B504e's UART goes here.
- **User button SW5:** requests a pipe in `pipe_client.sh`.

## 3. Wiring

**USB:** data-capable USB-C to USB-A cable from the breakout's **cellular modem** port to either stacked USB-A port on the DVK.

**UART:** three jumper wires from the breakout to the XBee socket:

| Breakout (B504e) | XBee socket pin | Signal |
|---|---|---|
| TX (D9) | 2 | DOUT (into the CC93) |
| RX (D10) | 3 | DIN (out of the CC93) |
| GND | 10 | GND |

Pins 1 to 10 are one column of the socket, with pin 1 (VCC) at the end with the angled corners; the DVK's silkscreen outline shows that shape. Pins 2, 3, and 10 are all in that column, and pin 3 is directly below pin 2. **Leave pin 1 (VCC) unconnected**, and don't power the breakout from the socket. Both sides are 3.3 V logic.

Tape or glue the wires once they're working. A wire on the wrong pin gives one-way communication that's easy to misdiagnose.

### Power-up order

All three power-up orders (breakout first, CC93 first, or both together) passed testing with the final setup. Breakout first remains the recommended order, since it was the one used for most of the testing:

1. Make sure the CC93 is completely unpowered (barrel connector unplugged). The modem USB cable between the breakout and the CC93 can stay connected.
2. Power the breakout, and wait for the B504e to connect to the cloud (breathing cyan, up to about a minute).
3. Plug in the CC93's barrel connector.

**Power down in the reverse order:** the CC93's barrel connector first, then the breakout's power.

Background: a powered CC93 puts 5 V on the modem USB cable and 3.3 V on its UART transmit line. Before the deauthorize rule existed, powering the CC93 with the breakout off was once followed by the modem becoming unresponsive, with `Failed to power off modem` in the B504e log and repeated restarts. If that ever happens, remove all power, wait 30 seconds, and power up in the order above.

A reset of the B504e (reset button or `System.reset()`) doesn't power-cycle the modem either. If the modem seems stuck, remove power rather than pressing reset.

## 4. Set up the Mac

Install the Particle CLI:

```
bash <( curl -sL https://particle.io/install-cli )
```

Open a new Terminal window, then:

```
particle --version
particle login
```

That's all the Mac needs. `screen`, used for the serial consoles, is built into macOS, and no FTDI driver is needed on Apple Silicon.

## 5. Serial consoles

Use two Terminal windows: one for the CC93 console, one for the B504e.

### Find the ports

Use `/dev/cu.*` devices, not `/dev/tty.*`. On macOS, `tty.` devices can block waiting for a carrier signal and look frozen.

```
ls /dev/cu.usbmodem*
```

With both boards connected, you'll see two entries. To tell them apart, unplug the B504e's MCU cable and run the command again: the one that disappears is the B504e. The numbers can change between sessions.

### CC93 console

```
screen /dev/cu.usbmodemXXXXX 115200
```

Press Enter, and log in as `root` (no password on a stock DEY image).

### B504e

To send commands and see replies (typing doesn't echo):

```
screen /dev/cu.usbmodemYYYYY 115200
```

To watch logs, including across resets:

```
particle serial monitor --follow
```

To exit `screen`: **Ctrl-A, then K, then Y.** To exit the monitor: **Ctrl-C.**

**Only one program can hold a port at a time.** If `screen` exits immediately with "[screen is terminating]", the port is in use, usually by `particle serial monitor` in another window. Stop it first.

**Tip:** paste commands into the CC93 console one at a time. Several lines at once can interleave with the board's output and hide errors.

## 6. Configure the CC93

These are one-time steps. Everything persists across reboots.

### Disable ModemManager

ModemManager must never touch the modem. With it running, Device OS completes its cloud handshake but never finishes connecting.

```
systemctl disable --now ModemManager
```

### Disable the CCCS demo

The ConnectCore Cloud Services demo in the stock image claims the user button.

```
systemctl disable --now cccs-gs-demo
```

### Copy the host files

The CC93 has no network until a pipe opens, so the simplest way to copy files is to paste them into the console. For each file below: paste the `cat` line, press Enter (the prompt changes to `> `), paste the file's full contents from this repo, then type `EOF` on its own line and press Enter. If the CC93 has Ethernet or Wi-Fi access to your Mac, `scp` works too.

> Root's home directory on DEY is `/root`, not `/home/root`. Using `~` avoids the difference.

**udev rule** (contents of `host/90-eg91-ecm.rules`):

```
cat > /etc/udev/rules.d/90-eg91-ecm.rules << 'EOF'
```

**Deauthorize rule** (contents of `host/89-eg91-deauthorize.rules`):

```
cat > /etc/udev/rules.d/89-eg91-deauthorize.rules << 'EOF'
```

**DHCP service** (contents of `host/eg91-dhcp@.service`):

```
cat > /etc/systemd/system/eg91-dhcp@.service << 'EOF'
```

**Pipe client** (contents of `host/pipe_client.sh`):

```
cat > ~/pipe_client.sh << 'EOF'
```

Then:

```
chmod +x ~/pipe_client.sh
```
```
systemctl daemon-reload
```
```
udevadm control --reload
```

Apply the deauthorize rule to the modem that's already connected:

```
udevadm trigger --action=add --subsystem-match=usb
```

Check that the script pasted intact:

```
wc -l ~/pipe_client.sh; tail -3 ~/pipe_client.sh
```

The line count should match the repo copy (`wc -l host/pipe_client.sh` on the Mac), and the file should end with `done`.

### What these do

- The **deauthorize rule** keeps the modem's USB disconnected from the CC93 by default, even with the cable plugged in. While the CC93 has the modem's USB configured, the modem stops answering the B504e reliably, so the CC93 only connects to it during a pipe.
- The **ECM rule and DHCP service** run DHCP on the modem's `usb0` interface whenever it appears. The B504e only puts the modem in ECM mode during a pipe, so `usb0` (and a cellular route) exists only while the CC93 has the modem.
- **`pipe_client.sh`** listens on `/dev/ttyXBee`. On `PIPE READY`, it authorizes the modem's USB, waits for `usb0`, syncs the clock over it (the CC93 has no network outside a pipe, so its clock starts out wrong), and after the download puts the modem back in its default USB mode over the modem's AT port, downloads test data (two 50 MB chunks by default), and sends `PIPE DONE` with the result and throughput. Pressing **SW5** requests a pipe.

## 7. Set up the cloud

### Product

Add the B504e to a Particle product if it isn't already. Environment variables and the firmware's OTA handling depend on it.

### Ledger definitions

In the console, create both Ledgers. Names must match exactly.

| Name | Type | Scope |
|---|---|---|
| `pipe-request` | Cloud to device | Device |
| `pipe-status` | Device to cloud | Device |

The `pipe-status` instance is created automatically when the device first writes to it. The `pipe-request` instance for the device is created in [section 10](#10-run-a-pipe), since setting it is what opens the first pipe.

### Environment variables

In the product, create these environment variables with the following values:

| Name | Value | Purpose |
|---|---|---|
| `PIPE_MAX_MINUTES` | `20` | How long a pipe stays open after `PIPE READY` without `PIPE DONE` |
| `PIPE_SETTLE_S` | `60` | Wait after the modem is confirmed in ECM mode, before `PIPE READY` |
| `UART_BAUD` | `115200` | Baud rate of the UART to the host. The host must match. |

These match the firmware's built-in defaults, so the device behaves the same either way. Setting them explicitly makes the values visible in the console and lets you tune them per product or per device without new firmware. See [PROTOCOL.md](PROTOCOL.md#environment-variables) for the allowed ranges.

After setting them, use **Save changes and apply**. Environment variables are delivered as an OTA update and read at boot, so the device picks them up on its next restart.

## 8. Flash the B504e

Connect the B504e's **MCU** USB-C port to the Mac and confirm the CLI sees it:

```
particle usb list
```

It reports as `B5-SoM`; the build platform is `b5som`. Check its Device OS version:

```
particle identify
```

Build against that version (6.4.0 or later) from the repo's `firmware` directory:

```
cd firmware
particle compile b5som src/main.cpp --target 6.4.1 --saveTo firmware.bin
```

Put the B504e in DFU mode: hold **MODE**, tap **RESET**, and release MODE when the LED blinks yellow. Then:

```
particle flash --usb firmware.bin
```

If the device's Device OS is older than the build target, it boots into safe mode (breathing magenta). Run `particle update` to bring Device OS up to date, then flash again.

Particle Workbench works as well: open the `firmware` folder, select `b5som` and Device OS 6.4.1, and use **Flash application (local)**.

## 9. Verify

### The B504e connects

Start the log before resetting the B504e:

```
particle serial monitor --follow
```

A normal boot looks like this:

```
[system] INFO: Cloud: connecting
...
[system] INFO: Cloud connected
[app] INFO: pipe-status: {"state":"normal"}
```

The LED ends at **breathing cyan**, and the `pipe-status` instance appears in the console with `"state": "normal"`.

A normal boot sends the modem no commands of its own, so the log looks like any Particle device's.

**Allow about a minute after a reset button press.** Pressing reset restarts the B504e but not the modem, and Device OS needs up to ~55 seconds to recover it (`No response from NCP`, then `mux` lines around 50 seconds in). The firmware's own resets can show the same recovery; it's normal.

If a pipe ended uncleanly (crash, watchdog, or a reset while closing), the next normal boot logs `modem may still be in ECM mode, restoring default before connecting` and fixes the modem first, adding about a minute. If power was lost mid-pipe, the firmware can't know, so after 3 minutes without a connection it logs `not connected after 3 min, checking modem USB mode`. If the modem was left in ECM, it restores default mode and resets (`fallback: modem restored to default mode, resetting`), and the next boot connects normally.

### The UART works in both directions

After every CC93 reboot, set the port before testing by hand (`pipe_client.sh` does this itself):

```
stty -F /dev/ttyXBee 115200 raw -echo
```

**CC93 to B504e:** run this on the CC93, and watch the B504e's `screen` window on the Mac for a `STATUS mode=normal cloud=yes ...` reply:

```
printf 'STATUS\n' > /dev/ttyXBee
```

**B504e to CC93:** on the CC93, run `timeout 30 cat /dev/ttyXBee`, then type `STATUS` in the B504e's `screen` window. The reply should appear on the CC93.

If only one direction works, one wire is on the wrong pin. See [section 3](#3-wiring).

### The modem is connected but deauthorized

The modem should be present on the CC93's USB bus, but deauthorized:

```
grep -l 2c7c /sys/bus/usb/devices/*/idVendor
```

That prints the modem's directory, for example `/sys/bus/usb/devices/1-1.1/idVendor`. Then:

```
cat /sys/bus/usb/devices/1-1.1/authorized
```

Expect `0`. `ls /dev/ttyUSB*` should find nothing, and `ip addr show usb0` should report the device doesn't exist. If `grep` prints nothing, check the modem cable.

## 10. Run a pipe

Close the B504e's `screen` window if it's open; the B504e needs nothing from the Mac during a pipe. Start the client on the CC93:

```
~/pipe_client.sh
```

It prints `listening on /dev/ttyXBee` and `button watcher on gpiochip3 line 23`.

### From the cloud: create the `pipe-request` instance

In the console, open the `pipe-request` Ledger, create an instance for your device, and add these rows in the Default view:

| Key | Value |
|---|---|
| `id` | `test-001` |
| `reason` | `bench-test` |

Saving it opens the first pipe as soon as the B504e syncs. For every pipe after that, edit the same instance and change `id` to a new value (`test-002`, `test-003`, ...); an `id` that's already been handled is ignored.

### From the edge

Press **SW5** on the DVK.

### What you'll see on the CC93

```
21:07:48 << MODE pipe id=test-004 reason=bench-test settle_s=60
21:09:19 << PIPE READY id=test-004 reason=bench-test max_minutes=20 job={}
21:09:19 pipe test-004 open, waiting for usb0
21:09:19 usb0 up at 192.168.225.27
21:09:21 clock synced (was 2025-12-16 19:52:07)
21:09:19 chunk 1/2: 52428800 bytes
21:10:27 chunk 1 done in 68s
21:10:27 chunk 2/2: 52428800 bytes
21:11:43 chunk 2 done in 75s
21:11:42 modem set to default USB mode
21:11:42 modem rebooting
21:11:43 >> PIPE DONE result=ok modem=restored detail=100MB in 144s 5.8Mbps
21:11:43 << PIPE CLOSING id=test-004 result=ok
21:11:43 << MODE normal
```

Several repeated `PIPE READY` lines before `PIPE CLOSING` are normal; they queued up during the download.

Shortly after `MODE normal`, the B504e is back online (breathing cyan), and `pipe-status` shows the result:

```json
{ "state": "normal", "last": { "id": "test-004", "result": "ok", "detail": "100MB in 144s 5.8Mbps", "duration_s": 234 } }
```

### 200 MB

```
CHUNKS=4 ~/pipe_client.sh
```

Each run downloads 200 MB. At about 5.8 Mbps, the download takes about 5 minutes.

Press **Ctrl-C** to stop the client. Wait for each pipe to finish completely (B504e breathing cyan) before starting the next.

## 11. Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `screen` exits with "[screen is terminating]" | Port in use, or wrong port | Stop `particle serial monitor`; check `ls /dev/cu.usbmodem*` |
| CC93 console shows nothing | Board already booted | Press Enter; reset the DVK |
| No `ttyUSB` devices on the CC93 | Charge-only cable, breakout unpowered, or the B504e resetting | Swap the cable; power the breakout; wait a minute |
| `modem usbnet: no answer` in pipe mode | Device OS still setting up the modem | Normal for a short while; the firmware backs off and retries |
| B504e blinking green indefinitely, `Failed to enter data mode` | Modem in ECM mode during normal operation | Fixed automatically at the next normal boot. If not, see [section 12](#12-manual-modem-reset). |
| B504e stuck after `Sending subscriptions` | ModemManager running on the CC93 | `systemctl disable --now ModemManager` |
| `No response from NCP` a few minutes into a session, vitals not updating | The CC93 has the modem's USB authorized outside a pipe | Check the deauthorize rule is installed; `cat /sys/bus/usb/devices/1-1.1/authorized` should print `0` |
| `Failed to power off modem`, B504e restarting every couple of minutes | Modem partially powered by the CC93, or left in a bad state | Remove all power, wait 30 s, then follow the [power-up order](#power-up-order) |
| UART works in one direction only | A wire on the wrong pin | Check pins 2, 3, and 10 ([section 3](#3-wiring)) |
| Manual UART tests show nothing after a CC93 reboot | Port settings reset | `stty -F /dev/ttyXBee 115200 raw -echo` |
| `pipe_client.sh: No such file or directory` | File went to `/home/root` | Root's home is `/root`; use `~/pipe_client.sh` |
| `button watcher stopped` right at startup | Button line in use | `systemctl disable --now cccs-gs-demo`; make sure only one copy of the script is running |
| `pipe-request` change ignored | That `id` was already handled | Use a new `id` |
| New environment variable value not used | Read only at boot | Apply it in the console; it takes effect at the next restart |
| Cloudflare returns `403 Forbidden` | Requests of 100 MB or more are refused | Keep `CHUNK_BYTES` at 50 MB; use `CHUNKS` for more data |
| Traffic stops mid-download | SIM data cap reached | Check usage with your SIM provider |

## 12. Manual modem reset

The firmware manages the modem's USB mode, so this shouldn't be needed. But if the modem ever needs to be put back in default mode by hand (for example, to run other firmware), from the CC93, first authorize the modem's USB (wait for `ls /dev/ttyUSB*` to list its ports):

```
echo 1 > /sys/bus/usb/devices/1-1.1/authorized
```
```
microcom -s 115200 /dev/ttyUSB2
```

Send `AT`, and wait for `OK` before anything else. Then `AT+QCFG="usbnet",0`, wait for `OK`, then `AT+CFUN=1,1`. Exit with Ctrl-X. The modem re-enumerates after `AT+CFUN=1,1`, and the deauthorize rule disconnects it from the CC93 again automatically.

If Device OS keeps power-cycling the modem so that `/dev/ttyUSB2` disappears before you can open it, this waits for it:

```
until [ -e /dev/ttyUSB2 ]; do sleep 0.5; done; microcom -s 115200 /dev/ttyUSB2
```
