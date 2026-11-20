# RFID Reader (Arduino Due + NXP PN7160)

Firmware for an Arduino Due that watches up to five NXP PN7160/PN7161 NFC
readers and reports tags to a PC over USB.

* **HID:** one key press per tag event, looked up in the reader/tag map (see
  "Tag map" below). With `set output id` it sends `<reader>+<UID>` when a tag
  appears and `<reader>-<UID>` when it leaves instead, e.g. `1+04A1B2C3D4E5F6`.
* **CDC (virtual serial port):** debug telemetry and a configuration CLI.

Tag type doesn't matter: anything the PN7160 can poll (ISO 14443A/B, ISO 15693,
and FeliCa on old firmware) produces a UID. By default only ISO 14443A tags
(NTAG, MIFARE) are polled, which keeps each reader's turn short; `set tech ABV`
adds the others at the cost of slower response.

## Building and connecting

1. Arduino IDE, board **Arduino Due (Programming Port)** (install *Arduino SAM
   Boards* from the Boards Manager). No extra libraries are needed; `Wire` and
   `HID` come with the Due core.
2. Open `RFID_Reader.ino` and upload through the **Programming** port.
3. Connect the PC to the **Native USB** port. It shows up as a keyboard / HID
   device plus a serial port. Open the serial port in any terminal (Arduino
   Serial Monitor works; line ending "Newline") and type `help`. The sketch
   stays silent on the serial port until you first type something (Enter is
   enough), then prints "RFID Reader ready". Any baud rate
   works. The sketch turns off the Due's "1200 baud on the Native port means
   jump to the bootloader" behaviour, so the board can't get stuck there; the
   flip side is that uploads must go through the Programming port.
4. The on-board **L** LED blinks once a second while the sketch is running. If
   it stops blinking, the sketch has hung.
5. Optional run indicator: an LED on **pin 32** lights as soon as the sketch
   starts and stays lit while it runs. Wire pin 32 → 330 Ω to 1 kΩ resistor →
   LED anode, LED cathode → GND. Change `RUN_LED_PIN` in `RFID_Reader.ino` to use
   another pin.

## Wiring

The PN7160 has only two address pins (HIF1/HIF2), so it can only use I2C
addresses **0x28 to 0x2B** (datasheet section 11.2.1.1, table 14). Four readers fit
on one bus; the fifth needs the Due's second I2C bus. That's the default:

| Reader | Bus | SDA / SCL | Address (HIF2 HIF1) | IRQ pin | VEN pin |
|---|---|---|---|---|---|
| 1 | 0 | SDA 20 / SCL 21 | 0x28 (0 0) | 22 | 23 |
| 2 | 0 | SDA 20 / SCL 21 | 0x29 (0 1) | 24 | 25 |
| 3 | 0 | SDA 20 / SCL 21 | 0x2A (1 0) | 26 | 27 |
| 4 | 0 | SDA 20 / SCL 21 | 0x2B (1 1) | 28 | 29 |
| 5 | 1 | SDA1 / SCL1 (next to AREF) | 0x28 (0 0) | 30 | 31 |

Every row can be changed from the CLI (`reader 5 bus 0`, `reader 3 irq 40`, ...).

* **Logic levels:** the Due is 3.3 V. Power the PN7160's VDD(PAD) from 3.3 V.
* **Pull-ups:** bus 0 has 1.5 kΩ pull-ups on the Due board. Bus 1 (SDA1/SCL1) has
  none, so add about 2.2 to 4.7 kΩ to 3.3 V on both lines.
* **IRQ:** the line is read as a level (high means "packet waiting"), so no edge
  can be missed. If a reader may be unplugged, add a 100 kΩ pull-down so a
  floating pin doesn't look like a waiting packet.
* **VEN:** optional. With it wired, the firmware power-cycles a reader that stops
  answering. Without it, use `reader N ven none` and tie VEN high on the module.
* **I2C clock:** the default is 20 kHz. Four PN7160 boards on one bus add more
  capacitance than the chip's 100 pF rating, and at 100 kHz the readers stopped
  answering. Only raise it (`set i2c <hz>`) with fewer boards or short wiring.

## Tag map

The map turns "tag N presented at / removed from reader R" into one key. Rows
are readers, and each tag owns two columns: the first for presented (+), the
second for removed (-).

```
          tag1  tag2  tag3  tag4  tag5
          +  -  +  -  +  -  +  -  +  -
reader 1  q  w  e  r  t  y  u  i  o  p
reader 2  a  s  d  f  g  h  j  k  l  ;
reader 3  z  x  c  v  b  n  m  ,  .  /
reader 4  Q  W  E  R  T  Y  U  I  O  P
reader 5  A  S  D  F  G  H  J  K  L  :
```

All five rows exist whether or not that reader is connected. The first time a
tag the map doesn't know is presented, it takes the next free tag column, so the
first five different tags become tag1 to tag5. Tags beyond five send nothing.
The keys are sent alone, without Enter.

**Key tags.** Up to ten key tags, numbered 0 to 9. Type `key 0 learn` in the CLI
(`tag 0 learn` works too), then present the tag you want as key 0 at any reader.
Key tags never type anything. Presenting the same key tag twice within 5 seconds
(lift it off for at least the `remove` time, 0.5 s by default, in between) erases
the map, and the next five new tags are learned in order.

The five tag IDs and the key tags are saved in flash as they are learned, so they
survive power cycles. They're stored apart from the settings: `save`,
`defaults` and `load` don't touch them. `map` shows the map, the learned tags and
the key tags, `map clear` erases the map, and `key 3 clear` forgets key tag 3.
**Uploading a new sketch erases them** along with the settings.

## HID output

`set hid` picks the method:

* `kb` (default): types the map key, or in `id` mode the line followed by Enter
  (`set enter`), like a barcode scanner. It assumes a **US keyboard layout** on
  the PC, because `;`, `:`, `/` and `+` sit on different keys elsewhere. Caps Lock inverts the letter case.
* `raw`: one 32-byte vendor-defined input report (usage page 0xFF00, report ID 3)
  per event, holding the same line as zero-padded ASCII. This is independent of
  keyboard layout and focus, but the PC software has to open the HID device.
* `both` or `off`.

In `set output id` mode each line starts with the number of the reader that saw the tag:
`<reader><+|-><tag>`, for example `1+04A1B2C3D4E5F6` when a tag arrives on R1 and
`1-04A1B2C3D4E5F6` when it leaves. Each reader reports its own tags, so moving a
tag from R1 to R2 gives `1-…` then `2+…`.

## How detection works

The PN7160 speaks NCI 2.0. Once set up, every reader waits with its RF field
off, and the readers take turns: only one field is ever on, which keeps the
supply current down. On its turn a reader switches RF on and polls for `poll`
ms (default 50). If it finds a tag it records the UID straight away. Either
way it then switches RF off, and after `rfoff` ms (default 5) the next ready
reader takes its turn. With the defaults, one round of four readers takes
roughly 0.15 s.

A tag that stays put is seen again on every turn, and a tag that is removed
stops being seen. It is reported gone (`<reader>-UID`) once it has been
missed for `remove` ms (default 300) or two full rounds, whichever is longer.
Several tags on one reader are each tracked.

## CLI

Settings change immediately; `save` writes them to flash. **Uploading a new
sketch erases the flash, so saved settings go back to defaults.**

```
status                      readers, present tags and settings
tags                        tags currently present
scan                        list I2C devices on both buses (readers re-initialise after)
reset <1-5|all>             power-cycle and re-initialise reader(s)
reader <1-5> on|off
reader <1-5> bus <0|1>
reader <1-5> addr <hex>     0x28..0x2B
reader <1-5> irq <pin>
reader <1-5> ven <pin|none>
set remove <ms>             unseen this long -> '-' event (default 300)
set poll <ms>               RF-on time per reader turn (default 50)
set rfoff <ms>              gap between one reader's RF off and the next one's on (default 5)
set tech <letters>          any of A B F V (default A)
map                         show the tag map and the learned tags
map clear                   forget the 5 tags (key tags stay)
key <0-9> learn             next tag presented at any reader becomes key tag 0-9
key <0-9> clear             forget that key tag ('tag' works the same as 'key')
set output <map|id>         HID sends a map key (default) or '<reader>+<UID>'
set hid <kb|raw|both|off>
set enter <on|off>          id output: press Enter after each line
set kbdelay <ms>
set log <0-4>               0 off, 1 events, 2 info, 3 debug, 4 NCI trace
set i2c <hz>
save | load | defaults
hidtest <text>              send text over HID as if it were an event
nci <1-5> <hex bytes>       send a raw NCI packet (debug)
```

Disable readers you haven't connected (`reader 5 off`, then `save`) to keep
the log quiet. They're retried every 10 s otherwise.

## Staying up: the watchdog

The Due core's USB interrupt code waits forever in several places for the PC to
answer. If the PC stops in the middle of a USB request (sleep, USB power saving,
a port reset), the chip freezes inside that interrupt: the HID and CDC devices
disappear from the PC and only the Programming port (a separate chip) is left.

The sketch therefore runs the SAM3X hardware watchdog. The loop feeds it on every
pass, and if the loop stops for 8 seconds the board restarts and the native port
comes back by itself (reopen the terminal afterwards). `status` shows the uptime,
whether the watchdog is on, and why the board last started, for example
`watchdog (the loop froze, last in USB service)` or `reset pin (RESET button or
Programming port opened)`. The same line is printed when the CLI starts.

The USB link is watched as well, because it has also been lost while the loop
kept running. Once the PC has set the board up, if the PC stops sending USB
frames or leaves the board unconfigured for 10 seconds, the board disconnects from
USB and restarts, so the PC finds it again as a new device. It only does this
after the PC has set it up once, so a PC that is asleep or off doesn't cause
repeated restarts. The last-reset line then reads `USB link lost (...)`, and
`status` counts how often the PC stopped sending frames or unconfigured the board.

**Log on the Programming port.** Everything the log prints also goes out on the
Programming port at 115200 baud. Opening that port restarts the board, so open it
first, then leave the terminal running: if the native port drops, the lines
leading up to it are there (`USB: PC stopped sending frames`, `USB: no longer
configured`, ...).

**Two firmware races that dropped the native port (fixed).** Both are between
the main loop and the core's USB interrupt, and could happen while the board sat
idle:

1. *Endpoint repair during a bus reset.* The core switches the serial and HID
   endpoints off whenever it rejects a USB request, so `usbService()` switches
   them back on. A USB bus reset from the PC also switches them off, about
   10 ms before the core notices the reset. The repair used to run inside that
   window and re-enable the endpoints in the middle of the reset, so they came
   out of it in a state the PC hadn't set up, and re-enumeration could fail.
   The repair now waits until the PC has sent a frame since the endpoints went
   off (the PC sends none during a reset) and re-checks with interrupts off.
2. *Check, then send.* Serial and HID output checked that the endpoint was
   free, then called the core's send, which waits forever for the endpoint.
   If the USB interrupt switched the endpoint off between the two, the loop
   froze. Sends now go through `usbSendPacket()`, which checks and writes the
   packet in one step with interrupts off and never waits.

`status` shows how often the endpoints were repaired.

**USB trace.** The sketch records every USB bus reset, every control request
the PC sends (and whether the core rejected it), and suspend/resume. When the
PC un-configures the board, and again just before a restart, the log prints
these events as `USB trace` lines on both ports. If the PC has given up on
the board (unconfigured and no frames), it restarts after 1 s rather than 10 s.

## Troubleshooting: board shows up as "Bossa Program Port"

If plugging in the Native USB port gives "Atmel Bossa Program Port" (or
"Bossa"/SAM-BA) instead of the HID device and serial port, the SAM3X is
starting from its built-in bootloader instead of the sketch. That happens
when the chip's "boot from flash" flag has been cleared. The usual cause is
the Native port being opened and closed at 1200 baud (by a terminal or by
the IDE with the "Native USB Port" board selected). This sketch blocks that
from now on, but it can't set the flag back, because it never gets to run.

The sketch is normally still in flash. Set the flag and restart, with only
the Native port plugged in (replace COM13 with the Bossa port's number, and
the bossac version folder with the one on your PC):

```
"%LOCALAPPDATA%\Arduino15\packages\arduino\tools\bossac\1.6.1-arduino\bossac.exe" --port=COM13 -U true -b -R
```

This erases nothing. If the board still comes up as Bossa afterwards, the
flash itself was wiped (for example by the board's ERASE line), and the
sketch has to be uploaded again through the Programming port.

## Assumptions and limits

* Protocol details come from the PN7160/PN7161 data sheet (Rev. 4.2) and the
  public NCI 2.0 / NXP reference driver behaviour: header-then-payload I2C reads,
  `CORE_RESET` / `CORE_INIT` (NCI 2.0 form), NXP proprietary activation `2F 02`,
  and NXP's standard RF discovery map.
* The module's clock, TVDD and RF settings are left as stored in the PN7160's
  own EEPROM. Ready-made boards ship configured; a custom board may need NXP's
  settings written once (see NXP's PN7160 user manual and application notes).
* FeliCa reading was removed in PN7160 firmware after 12.50.0A, so `F` is off by
  default. Turning it on with newer firmware makes discovery fail.
* ISO 15693 UIDs are printed in the byte order the reader reports (LSB first), so
  they appear reversed compared to the usual `E0...` notation.
* The Due's Wire library can only move 32 bytes per transfer, which is too few for
  NCI, so `I2CBus.cpp` drives the TWI hardware directly.
* Written without hardware to test on. The NCI state machine was checked against
  a simulated PN7160, and the code compiles cleanly for the Cortex-M3.

## Code layout

| File | Purpose |
|---|---|
| `RFID_Reader.ino` | setup/loop, event formatting |
| `TagSensor.h` | the interface a reader driver implements |
| `Pn7160Reader.*` | PN7160 NCI driver and presence detection |
| `I2CBus.*` | register-level I2C for both TWI peripherals |
| `TagTracker.*` | turns sightings into `+`/`-` events |
| `TagMap.*` | reader/tag key map, learned tags and key tag in flash |
| `HidOut.*` | HID descriptor, keyboard typing, raw reports |
| `UsbOut.*` | USB sends that drop data instead of blocking when the PC isn't reading |
| `Cli.*` | serial command line |
| `Health.*` | watchdog, crash capture, last-reset reason |
| `Config.*` | settings and flash storage |
| `Log.*` | telemetry output |

To use a different reader chip, write a class implementing `TagSensor`
(report UIDs through the callback for as long as they're in the field) and
change the `readers` declaration in `RFID_Reader.ino`.
