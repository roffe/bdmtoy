# nonameadapter

! IMPORTANT ! IMPORTANT ! IMPORTANT ! IMPORTANT ! IMPORTANT ! IMPORTANT !
* Firmwares built / uploaded before 21/9/2022 must be updated to function with the current host code/apps.
* Firmware 1.0 fixes unreliable old-BDM reads and dumps (Trionic 5/7/8), address 0, and a USB hang, and is
  required by BDM Tool. The host apps log the firmware version and warn about older ones.
  What changed and how to flash it: [firmware/README.md](firmware/README.md)
* Firmware 2.0 adds a USB bootloader: flash `firmware/bin/bdmtoy-full.hex` over SWD once, and later
  versions go on over USB (BDM Tool, the GUIs' Update button, `debugtool --update`). Same link: [firmware/README.md](firmware/README.md)
* Firmware 2.1 keeps BKPT pulled up while idle, so a Trionic powered up with the adapter on it no longer comes up halted.
* Firmware 2.2 is a plain vendor USB device instead of a serial modem: Windows installs WinUSB for it by itself
  (no more Zadig), and Linux no longer makes a ttyACM of it.



Info will follow... Basically it's a oldbdm, newbdm, sbdm, jtag, nexus adapter ..thing.
I got really tired of having a bunch of different adapters for different targets so I started working on one to rule them all.

Currently supports:
* Old BDM: Trionic 5.2/5.5(Most replacement flash is supported too), 7, 8, 8 mcp.
* NEXUS: Confirmed working on mpc5566 but there is nothing supported due to their locked down nature.
* SBDM: SIU from 9-5 MY06+, SID from 9-5MY04-05 (Including their eeprom) and some toy code for a random BMW cluster I found in a box.
* New BDM: EDC16C39 main flash and eeprom.


The adapter is based around a simple stm32f103 board (with VERY ugly code atm).
Pinouts can be found in common.h inside the firmware folder.
Jumper cables works fine for SBDM but you really need a decent, preferably shielded, cable for old/new BDM since it can run at up to 12 MHz.

Currently only gui apps (Linux app pictured, there's also one for Windows)

Building the host apps:
* CLI (`host/cli/both`): `make`, needs libusb-1.0.
* Linux GUI (`host/gui/nix/bdmstuff`): Qt 6 and libusb-1.0, `cmake -B build && cmake --build build`.
* Windows GUI (`host/gui/win/bdmstuff`): Visual Studio 2026 with .NET desktop and C++/CLI (.NET) support,
  open `bdmstuff.sln`, x64. It is .NET 10 WinForms with LibUsbDotNet 3 (NuGet); the protocol core is the
  C++/CLI `namedll`, and `libusb-1.0.dll` is copied next to the exe. From firmware 2.2 Windows installs
  the WinUSB driver for the adapter by itself: no Zadig.
![alt text](/bdmstuff.png)

Ain't many wires to keep track of:
![alt text](/pinout.png)
