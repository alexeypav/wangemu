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
This project requires wxWidgets 3.1.7 to compile. Follow these steps:

1. **Download wxWidgets 3.1.7:**
   - Download the following files from https://www.wxwidgets.org/downloads/:
     - wxWidgets-3.1.7-headers.7z
     - wxMSW-3.1.7_vc14x_Dev.7z  
     - wxMSW-3.1.7_vc14x_ReleaseDLL.7z

2. **Install wxWidgets:**
   - Extract all files to `C:\wxWidgets-3.1.7`
   - Set environment variable: `WXWIN=C:\wxWidgets-3.1.7`
   - Restart Visual Studio

3. **Copy Runtime DLLs:**
   After compiling, copy the required DLLs from `C:\wxWidgets-3.1.7\lib\vc14x_dll\` to your output directory:

   **For Debug builds:**
   - wxbase317ud_vc14x.dll
   - wxmsw317ud_core_vc14x.dll
   - wxmsw317ud_adv_vc14x.dll

   **For Release builds:**
   - wxbase317u_vc14x.dll
   - wxmsw317u_core_vc14x.dll
   - wxmsw317u_adv_vc14x.dll

License
----------
Historically the source code has not had any explicit license,
but it has always been my intent that the code should be maximally
open.  To that end, henceforth all Wangemu code is released under the
MIT License.
