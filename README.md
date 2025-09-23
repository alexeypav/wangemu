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

Building on Windows
----------
This project uses **static linking** with wxWidgets 3.1.7 to create standalone executables that require no external DLLs. Follow these steps:

### Prerequisites
- Visual Studio 2019 or later with C++ development tools
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

License
----------
Historically the source code has not had any explicit license,
but it has always been my intent that the code should be maximally
open.  To that end, henceforth all Wangemu code is released under the
MIT License.
