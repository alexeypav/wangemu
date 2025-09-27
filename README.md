# Modified Wang Emulator

This is a modified version of the original wangemu, forked from [jtbattle/wangemu](https://github.com/jtbattle/wangemu), added support to run as a "Real Terminal Server/Wang 2200 Multi User System" for connecting terminals over the host's serial ports and running on linux without gui/wxwidget dependencies.

For the GUI/Original version; added a Terminal mode (select the 2336DW in the CPU menu) to use the host's serial ports to connect to a real Wang2200 system

> **Disclaimer**: I used AI assistance quite extensively, otherwise this wouldn't be possible or would take me "years" to do this.


## Run this on a Raspberry PI and connect a real Wang 2x36 terminal
* Build the ARM version on linux (see below for build instructions)
* Copy over the binary to the Raspberry Pi
* Plug in your serial adapters and connect terminal/s, up to 4 should work
* Run the executable `./wangemu-terminal-server-aarch64 --web-config`
* Initial config is 1 terminal and using /dev/ttyUSB0 - if you have a terminal connected there it should work already
* Configure the setup via web - http://(ip or hostname of the Pi):8080 - click apply and reset
* Settings should apply, any extra terminals should come up
* For information on configuration, check the original documentation [emu.html](http://www.wang2200.org/emu.html)
* wangemu.ini manual configuration should work the same, you will need to stop the emulator first otherwise it'll overwrite the .ini file on exit
* For automatic startup on boot, copy over the setup-startup.sh and start-wangemu.sh script, run sudo ./setup-startup.sh


###  Null modem wiring I've used for connecting a Terminal to the USB Serial Adaptor

| DB9 (PC/Emu) | DB25 (Wang 2X36DW) | Signal     | Notes                                     |
|--------------|--------------------|------------|-------------------------------------------|
| 1            | 20                 | DTR → DCD  | Tie Terminal Ready to Host Carrier Detect |
| 2            | 2                  | TXD → RXD  | Terminal TX → Host RX                     |
| 3            | 3                  | RXD ← TXD  | Terminal RX ← Host TX                     |
| 4            | 6, 8               | DSR/DCD ← DTR | Host Ready & Carrier Detect → Terminal  |
| 5            | 7                  | GND        | Common ground                             |
| 6            | 20                 | DTR → DSR  | Terminal Ready → Host                     |
| 7            | 5                  | CTS ↔ RTS  | Crossed handshaking                       |
| 8            | 4                  | RTS ↔ CTS  | Crossed handshaking                       |

## Terminal Server Version

- **Serial communication** - connect multiple physical Wang 2X36 terminals via tty or windows com ports - I've had good results using the Unitek usb -> serial adaptors
- **ARM64 support** - includes ARM build for Raspberry Pi, tested on Raspbian OS on a Pi Zero 2
- **Configuration** - Added a web-based configuration interface for basic config - INI file config works as before too

## Windows added Emulator Features

- **Serial communication** over the host's COM ports
- **Terminal emulation** - use the emulator as a Wang terminal connected to a real Wang 2200 system  
- **Physical terminal support** - connect a Wang terminal via the host's COM port to the emulator

Building on Windows
----------

### Prerequisites
- Visual Studio 2017 or later with C++ development tools (C++17 support required)
- Developer Command Prompt for VS

### 1. Download and Extract wxWidgets Source
```bash
# Download wxWidgets 3.1.7 source code from:
# https://github.com/wxWidgets/wxWidgets/releases/tag/v3.1.7
# Extract to: C:\wxWidgets-3.1.7
```

### 2. Set Environment Variable
```bash
# Set WXWIN environment variable (required)
set WXWIN=C:\wxWidgets-3.1.7

# Or set permanently via System Properties > Environment Variables
# Then restart Visual Studio
```

### 3. Build wxWidgets Static Libraries

Open **Developer Command Prompt for VS** and run:

```bash
cd C:\wxWidgets-3.1.7\build\msw

# Clean any previous builds
nmake -f makefile.vc clean

# Build static release libraries
nmake -f makefile.vc SHARED=0 UNICODE=1 BUILD=release RUNTIME_LIBS=static

# Build static debug libraries
nmake -f makefile.vc SHARED=0 UNICODE=1 BUILD=debug RUNTIME_LIBS=static
```

### 4. Verify Build
```bash
# Check that static libraries were created
dir C:\wxWidgets-3.1.7\lib\vc_lib\*.lib

# Verify setup.h files exist
dir C:\wxWidgets-3.1.7\lib\vc_lib\mswu\wx\setup.h
dir C:\wxWidgets-3.1.7\lib\vc_lib\mswud\wx\setup.h
```

### 5. Build wangemu
- Open `wangemu.sln` in Visual Studio
- Select Debug or Release configuration
- Build the project

The resulting `wangemu.exe` will be **completely standalone** - no DLL files needed for distribution!

### Clean Commands
```bash
# Clean only debug build
nmake -f makefile.vc clean BUILD=debug

# Clean only release build  
nmake -f makefile.vc clean BUILD=release

# Clean everything
nmake -f makefile.vc clean

# Clean wangemu build artifacts
del Release\*.obj Release\*.pdb Release\*.pch
del Debug\*.obj Debug\*.pdb Debug\*.pch
```

### Troubleshooting
- **"Cannot open include file: 'wx/setup.h'"**: wxWidgets not built yet - run step 3
- **Runtime library mismatch errors**: Ensure `RUNTIME_LIBS=static` was used in step 3
- **"target does not exist" errors**: Clean completely and rebuild from step 3

Building on Linux
----------

### Prerequisites
- GCC 7+ or Clang 5+ (C++17 support required)
- Build tools: make, g++

### GUI Version (requires wxWidgets)
```bash
# Install wxWidgets development packages
sudo apt update
sudo apt install libwxgtk3.0-gtk3-dev build-essential

# Build GUI emulator
make         # Default debug build
make debug   # Debug build with symbols
make opt     # Optimized release build
```

### Terminal Server (headless, no GUI dependencies)
```bash
# Build x86_64 terminal server
make -f makefile.terminal-server         # Default debug build
make -f makefile.terminal-server debug   # Debug build with symbols
make -f makefile.terminal-server opt     # Optimized release build

# Build ARM64 for Raspberry Pi (requires cross-compiler)
sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
make -f makefile.terminal-server-aarch64         # Default debug build
make -f makefile.terminal-server-aarch64 debug   # Debug build with symbols
make -f makefile.terminal-server-aarch64 opt     # Optimized for Cortex-A53

# Run terminal server
./wangemu-terminal-server --web-config           # x86_64 version
./wangemu-terminal-server-aarch64 --web-config   # ARM64 version

# Command-line options
--ini=PATH          # Load configuration from specific INI file (default: wangemu.ini)
--web-config        # Enable web configuration interface on port 8080
--web-port=PORT     # Web server port (default: 8080, enables web interface)
--help, -h          # Show help message

# Clean build artifacts
make -f makefile.terminal-server clean
make -f makefile.terminal-server-aarch64 clean
```



Wang 2200 Emulator
==================

wangemu is able to emulate a Wang 2200, 2200VP, or 2200MVP computer.

The emulator is written in C++ and makes use of the
[wxWidgets](http://www.wxwidgets.org)
library, which allows it to compile and run both under Windows and MacOS.  In
theory it could run under Linux too, but I haven't tried it and likely there
will be some code tweaks to get it to compile.

Wangemu allows building a system configuration, including

* CPU type (2200B, 2200T, 2200VP, 2200MVP, MicroVP)
* amount of system RAM
* what type of peripheral is loaded into each of the backplane slots

The 2200 had an incredible array of peripherals it supported, but only a
few are emulated:

* CRT controller (64x16 or 80x24)
* MXD terminal mux and 2236 intelligent terminal
* printer controller (virtual, or redirect to real printer)
* keyboard controller
* disk controller

The emulator allows entering programs manually via the keyboard,
or sourcing them from a text file, or loading it off of a virtual
disk image.

The wvdutil/ subdirectory contains a python program for inspecting
and manipulating virtual disk images.  See the readme file under
that directory for more details.

History
----------
Wangemu has been developed on and off since 2002, advancing from
a barely functioning revision 0.5 to the current revision 3.0.
Jim Battle is the primary author of the emulator, but Paul Heller
was responsible for initially getting the emulator to work under
MacOS and adding printer support.

wangemu is just one tiny part of an extensive website concerning
the Wang 2200 computer, located at
[http://www.wang2200.org](http://www.wang2200.org).
Traditionally, on each release, a zip file containing the source
code, some notes, and a precompiled binary has been published to
the website's [emu.html](http://www.wang2200.org/emu.html) page.

License
----------
Historically the source code has not had any explicit license,
but it has always been my intent that the code should be maximally
open.  To that end, henceforth all Wangemu code is released under the
MIT License.
