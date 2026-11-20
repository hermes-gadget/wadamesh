# Remote-test controller (wadamesh-remote fork)

A serial control surface for the touch UI, ported from SigurdOS's
`SIGURDOS_REMOTE_TEST` build. It exists so a host can drive a real T-Deck the way
the SigurdOS hardware-test harness does: navigate, tap, type, read status, dump
the widget tree, change radio params, and pull screenshots — no hands on the
device.

Everything lives behind `-D WADAMESH_REMOTE_TEST=1`; release builds are
byte-for-byte the upstream behaviour.

- Firmware module: `src/remote_test/`
- Host client: `tools/remote_test/rt.py`
- Build env: `LilyGo_TDeck_companion_radio_touch_remote`
- Parser selftest: `tools/remote_test/parser_selftest.cpp`

## Build and flash

```bash
pio run -e LilyGo_TDeck_companion_radio_touch_remote
```

The T-Deck needs the usual NVS-preserving 4-component chain (bootloader /
partitions / boot_app0 / firmware at `0x0 / 0x8000 / 0xe000 / 0x10000`) —
flashing the merged image 0xFF-pads and wipes saved Wi-Fi credentials.

## Host client

```bash
pip install pyserial pillow           # pillow optional (PPM fallback)
./tools/remote_test/rt.py -p /dev/ttyACM0 status
./tools/remote_test/rt.py -p socket://hermes-portable:2000 nav home
./tools/remote_test/rt.py -p /dev/ttyACM0 tap 160 120
./tools/remote_test/rt.py -p /dev/ttyACM0 type "hello mesh"
./tools/remote_test/rt.py -p /dev/ttyACM0 capture home -o shots/
./tools/remote_test/rt.py -p /dev/ttyACM0 -i
```

`-p` accepts any pyserial URL, so the same tool works over a `ser2net` bridge.

## Commands

| Command | Effect |
|---|---|
| `help`, `?` | Command list |
| `ping` | `pong <uptime_ms>` |
| `version` | Controller build stamp |
| `screen` | Current tab / count / screen-off / lock state |
| `nav <0..N\|chats\|contacts\|home\|map\|sensors\|settings>` | Switch bottom tab |
| `tab next\|prev` | Cycle tabs |
| `back` | Go to Home |
| `tap <x> <y> [hold_ms]` | Pointer press/release through the web-pointer indev |
| `key <enter\|backspace\|esc\|tab\|up\|down\|left\|right\|space\|delete>` | One key via the hardware-key path |
| `type <ascii text>` | Type printable ASCII (3 chars/loop pass) |
| `status` | Heap, PSRAM, uptime, tab, lock state, radio params |
| `tree`, `widgets` | Bounded LVGL widget tree (class, pos, size, flags) |
| `capture [name]` | 16-bit BMP screenshot over serial (`<<<WMSHOT>>>` framing) |
| `setrf <freq> <bw> <sf> <cr> <dbm>` | Apply + persist radio params |
| `display on\|off` | Wake / soft-sleep the panel |
| `lock`, `unlock`, `wake` | Screen lock controls |
| `reboot` | Restart |

Responses are prefixed `[rt] ` so they can be filtered out of normal boot/mesh
chatter.

### Input safety

The line reader only consumes input when the first byte is an ASCII letter. A
companion-protocol frame starts with `<` or a binary command byte, so the phone
app / serial protocol is never starved. Text commands are not consumed while
the mesh is in CLI-rescue mode (`<`-prefixed), and the reader never echoes
binary data back.

### Capture format

`capture` arms the firmware's existing screenshot mirror (`g_shot_buf`), forces
a full redraw, then streams a 16-bit BI_BITFIELDS BMP:

```
<<<WMSHOT name=<name> w=320 h=240 bytes=<n>>>
<BMP header + bottom-up RGB565 rows>
<<<WMEND>>>
```

This is the same framing the upstream `scripts/doc/capture.py` uses, so
existing tooling keeps working. `rt.py` converts to PNG when Pillow is
installed, otherwise writes a binary PPM.

## Upstream touch points (keep these tiny)

The fork is designed to rebase onto `ALLFATHER-BV/wadamesh` with no manual
conflict resolution. Only three upstream files change, each marked with
`[wadamesh-remote]` fences:

1. `src/main.cpp` — one include, one `remote_test::begin()` after
   `ui_task.begin()`, one `remote_test::loop()` in `loop()`.
2. `src/ui-touch/UITask.h` — three declarations in the public section.
3. `src/ui-touch/UITask.cpp` — three shim definitions appended at EOF.

`platformio.ini` gets one extra `[env:...]` block at the end. Everything else
is new files under `src/remote_test/`.

Rebase:

```bash
git fetch upstream
git rebase upstream/main
scripts/rebase-upstream.sh      # verifies the hooks survived, then builds
```

If a hook conflicts, keep upstream's code and re-add the fenced block — the
blocks are self-contained and do not reorder upstream logic.

## Verification

- Host parser selftest: `tools/remote_test/parser_selftest.cpp` (g++, no hardware)
- Firmware build: `pio run -e LilyGo_TDeck_companion_radio_touch_remote`
- On-device: flash, open the serial console, `help` → `screen` → `nav home` →
  `capture boot` and confirm the PNG matches the panel.
