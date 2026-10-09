# bdmtoy firmware

STM32F103 (Blue Pill class board, 48 MHz). Pinout in `common.h`; BDM is on
port B (PB11 RESET, PB12 FREEZE, PB13 DSCLK/BKPT, PB14 DSO, PB15 DSI).
PB13 has a pull-up while idle (2.1).

## Firmware 2.2

The adapter is a vendor-specific USB device now, no longer a CDC-ACM modem:
one interface (0) with the endpoints it always had, EP 0x03 out and 0x81 in.
The hosts only ever spoke raw frames on those over libusb, never used it as a
serial port.

- **Windows** loads WinUSB for it on its own, through Microsoft OS 2.0
  descriptors (Windows 8.1 and later): no Zadig. Device interface GUID
  `{8351DC3D-AA28-494A-85EE-A752F61F7157}`; the bootloader has its own.
- **Linux** binds no driver to it: no `/dev/ttyACM`, nothing to detach, and
  ModemManager no longer probes it with AT commands. It still needs a udev
  rule for access, see below.
- **Hosts** claim interface 0. They also try interface 1, which only the CDC
  layout of earlier firmware has, so such an adapter still answers its
  version and is told to update.

Checked on Linux: the BOS and descriptor set Windows reads (compatible ID
WINUSB, the GUID), and on a T7 the updaters, a dump, and an erase and write.
The automatic driver install itself still wants a first plug-in on Windows.

## Firmware 2.1

DSCLK/BKPT (PB13) is pulled up (the STM32's internal ~40 kΩ) whenever the
adapter is idle, from power-up on; it used to float. A CPU32 samples BKPT as
RESET rises, and the T7 does not pull it up itself: an ECU powered up with an
idle adapter on it could come up halted in BDM. Bench, T7: an ECU power cycle
with 2.1 idle on it comes up out of BDM (FREEZE low), and a dongle restart
with the ECU running leaves it running.

On MPC5xx (new BDM, EDC16) the same pin is DSCK, and DSCK *high* at reset
enters debug mode. Only the time before a host first picks new BDM sees the
pull-up; new BDM floats the line as before. Untested on an EDC16.

## Firmware 2.0

2.0 adds a USB bootloader, so after one flash over SWD the adapter is updated
over USB, and moves the firmware onto current libraries. The BDM side is 1.0's.

- **Bootloader**: USB DFU 1.1 in the first 16 KB of flash, see below.
- **USB**: [TinyUSB](https://github.com/hathach/tinyusb) 0.21 in place of ST's
  2011 USB-FS library. Same descriptors (ffff:0107, CDC-ACM, EP 0x81 in, 0x03
  out, 0x82 notify) until 2.2 made it a vendor device. Each reply frame
  goes out as one transfer, with a zero-length packet when it ends on a full
  packet.
- **Libraries**: CMSIS 6, ST's current STM32F1 device headers and LL drivers
  in place of the Standard Peripheral Library; built with arm-none-eabi-gcc 16
  and newlib-nano. Versions in [`lib/VERSIONS.md`](lib/VERSIONS.md).
- `TAP_DO_BOOTLOADER` (0x0004) reboots into the bootloader; the serial number
  is the chip's unique ID.

### Flash layout

| Address | Size | Holds |
|---|---|---|
| 0x08000000 | 16 KB | bootloader (`bin/bootloader.*`) |
| 0x08004000 | 48 KB | app (`bin/firmware.*`), vector table first |
| 0x20004FFC | 4 bytes RAM | boot request word, left out of both images' RAM |

The bootloader starts the app straight from reset unless the app asked for
the bootloader, the BOOT1 jumper is at 1, or there is no valid app. It then
enumerates as **ffff:0108** ("bdmtoy bootloader") with the PC13 LED on.

It takes an app image (`bin/firmware.bin`, linked for 0x08004000) in 1 KB
blocks. The app's first page, its vector table, is written last: an update
that is cut off (unplugged, host crash) leaves no valid app, and the adapter
comes back up in the bootloader to be updated again. After a complete
download a DFU_DETACH or a USB reset starts the new app.

### Updating over USB

Any of these takes `bin/firmware.bin` (BDM Tool carries its own copy):

- [BDM Tool](https://github.com/roffe/bdmtool): Firmware → bdmtoy.
- The GUIs: the **Update** button under "Adapter".
- The CLI: `debugtool --update bin/firmware.bin`.
- dfu-util, with the adapter already in the bootloader (BOOT1 at 1, or after
  any of the above was cut off): `dfu-util -d ffff:0108 -D bin/firmware.bin -R`.

The tools reboot the running app into the bootloader, write the image, start
it and report the new version. Firmware before 2.0 has no bootloader, so it
needs the SWD flash below once.

**Linux** needs access to the app's (0107) and the bootloader's (0108) USB
IDs, as root:

```sh
cat > /etc/udev/rules.d/71-bdmtoy.rules <<'RULE'
ACTION=="add", SUBSYSTEM=="usb", ATTR{idVendor}=="ffff", ATTR{idProduct}=="0107", MODE="0660", TAG+="uaccess"
ACTION=="add", SUBSYSTEM=="usb", ATTR{idVendor}=="ffff", ATTR{idProduct}=="0108", MODE="0660", TAG+="uaccess"
RULE
udevadm control --reload-rules
```

**Windows** installs WinUSB by itself for both the app (2.2 and later) and
the bootloader (Microsoft OS 2.0 descriptors): no Zadig.

**Recovery**: if the adapter does not show up at all, set the BOOT1 jumper to
1 and plug it in: it stays in the bootloader whatever the app. Set BOOT1 back
to 0 after the update.

## Firmware 1.0

1.0 is the first versioned release and the one [BDM Tool](https://github.com/roffe/bdmtool)
requires. It fixes the old-style CPU32 BDM path (Trionic 5/7/8, CANdi) and the
USB link. The host apps in this repository work with it and say which
firmware the adapter has: the CLI when it connects, the GUIs when they start
and again whenever "Select target" is picked, and an operation if it finds a
different adapter than was last reported. Older firmware gets a warning.

| Problem in earlier firmware | Symptom | 1.0 |
|---|---|---|
| A USB packet that arrived while a command was still running was neither read nor re-armed | EP3 NAKed for good: the adapter stopped answering until replugged. Most likely right after a dump, which does one more BDM read after its last frame | The packet waits in the PMA (host held off by NAK) and is taken when the command is done |
| Frame length taken from the first packet without a bounds check | A stray write to the CDC-ACM tty (AT commands) ran past `receiveBuffer` | Lengths outside 8..2048 bytes are dropped; a USB bus reset clears a half-received frame |
| Address 0 meant "no address" in `BDMOLD_ExecRead/ExecWrite` | Reads at 0 failed, dumps from 0 got their first long word wrong, writes to 0 went elsewhere | Commands carry an address when they are memory READ/WRITE, whatever its value |
| After each SPI word the pins went back to GPIO with DSCLK low | The next BDM frame started while the target was still taking in the last command. With slow memory (the T7 prep's 13 wait states) long-word reads failed and dumps had wrong long words: a few per 512 KB at 1 MHz, faults within a millisecond at 1.5-4 MHz | DSCLK idles high between frames; a gap after each command frame (`frame_gap`) and a settle time before the status bit is read |
| Turbo loops treated any status-1 reply as "not ready" | Bus errors and illegal-command replies were retried or turned into data | "Not ready" (status 1, data 0) is retried up to a limit; anything else ends the dump/fill with a fault |
| Not-ready loops and the wait after GO had no limit | An unmapped address with the bus monitor off hung the adapter until power-cycled | 50 ms limit (`RET_MAXRETRY`); GO gives up after 100 ms (`RET_NOSTART`) |
| `TARGETREADY` reset only if asserting BKPT did not halt the target | A target already halted, or running with BDM enabled, kept its setup, including write-once SYPCR/TRAMBAR | Always pulses RESET with BKPT held |
| `TARGETSTOP` was `TARGETREADY` | No way to halt without a reset | BKPT only; `RET_NOTREADY` if the target does not halt |
| `TARGETRESET` floated BKPT as RESET rose | Targets without a BKPT pull-up (T7) came up with BDM enabled | BKPT is driven high across the reset and left high |
| A dump of one long word left the turbo loop with a count of 0 | Ran about 2^32 times, past the send buffer | Only entered with work to do; the last long word sends NOP, so nothing is left running (the old trailing "thrash" read is gone) |
| A dump that could not start (malformed, no interface) sent nothing | The host waited for its timeout | Answered with a refusal frame |
| `ReadMemory` sized each read from the total length | A 6-byte read returned 8 bytes | Sizes from what is left; replies over the buffer are refused |
| `FillMemory` padded a short tail with zeros | Wrote past the received data, into the target too | Whole long words only (`RET_MALFORMED` otherwise); the data must all be in the frame |
| A reply ending on a full 64-byte packet was split 62 + 2 for one size only | A host reading into a larger buffer could wait for more | Any frame ending on a full packet is followed by a zero-length packet |

## Protocol additions

All words are little-endian; see `shared/enums.h` and `shared/cmddesc.h`.

- `TAP_DO_VERSION` (0x0002): no arguments, answers one word, `major << 8 |
  minor` (`ADAPTER_FW_VERSION`). Works before `TAP_DO_SETINTERFACE`. Earlier
  firmware answers `RET_NOTSUP` (0xF000), which is how a host tells them apart.
- `TAP_DO_BDMTIMING` (0x0003): `[settle ns][gap ns]`, old-BDM frame timing:
  time from DSCLK falling to sampling the status bit, and the pause after a
  frame that hands the target a command. Defaults 1500 / 1000 ns, restored
  by every `TAP_DO_SETINTERFACE`. Only for odd targets or cables.
- Dump faults: a dump that fails part way sends `[5][0x0050][status][addr lo][addr hi]`
  in place of the next data frame, `addr` being the long word that failed. One
  that cannot start sends `[3][0x0050][status]`.
- Old-BDM status codes now reported: `RET_BUSTERMERR` (bus error),
  `RET_ILLCOMMAND`, `RET_MAXRETRY` (still not ready after 50 ms), `RET_NOTREADY`
  (target did not enter BDM), `RET_NOSTART`.
- Every frame gets exactly one reply frame, and the host must wait for it
  before sending the next (the firmware NAKs until then).

## Timing (old BDM)

Bench, Trionic 7 at 16.78 MHz with its flash on 13 wait states: dumps are
byte-exact at every clock from 500 kHz to 6 MHz (12 MHz is past the CPU32's
DSCLK limit of half the CPU clock and fails). 512 KB takes 2.1 s at 6 MHz,
7 s at 1 MHz. Start at a slow clock (1 MHz) out of reset, when a 68332 runs
at 8.4 MHz, and go faster once the target's clock is set.

## Building and flashing

Needs `arm-none-eabi-gcc`. `make` builds:

- `bin/firmware.{elf,bin,hex}`: the app, for updates over USB
- `bin/bootloader.{elf,bin,hex}`: the bootloader
- `bin/bdmtoy-full.hex`: both, for the first flash

The first time, flash `bin/bdmtoy-full.hex` over SWD (PA13 SWDIO, PA14
SWCLK) with BOOT0 = 0. `make flash` does it with an ST-LINK and OpenOCD
(`make flash-app` writes the app only). Back up the old firmware first:

```sh
openocd -f interface/stlink.cfg -c "transport select hla_swd" -c "set CPUTAPID 0" \
    -f target/stm32f1x.cfg -c init -c halt \
    -c "dump_image backup.bin 0x08000000 0x10000" -c shutdown
make flash
```

STM32CubeProgrammer works too, with `bin/bdmtoy-full.hex`. `flash.cfg` does
the same with a picoprobe.

If SWD will not connect while the dongle is cabled to a powered ECU, power
the ECU down for the flash (seen on a T7 bench: it connects with the ECU off).
