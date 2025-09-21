# Serial Terminal Setup Guide

## Overview

The Wang 2200 emulator now supports redirecting MXD Terminal Mux terminals to real COM ports, allowing physical terminals to connect to the emulated Wang 2200 system.

## Configuration

### Step 1: Configure the Terminal Mux Card

In your `wangemu.ini` file, you need to add a Terminal Mux card (type `7079`) and configure one or more of its terminals to use COM ports.

Example configuration:

```ini
[wangemu/config-0/io/slot-3]
type=7079
addr=0x215

[wangemu/config-0/io/slot-3/cardcfg]
numTerminals=2
terminal0_com_port=COM3
terminal0_baud_rate=19200
terminal0_flow_control=0
terminal1_com_port=
terminal1_baud_rate=19200
terminal1_flow_control=0
```

### Configuration Parameters

- `numTerminals`: Number of terminals (1-4)
- `terminal0_com_port`: COM port name for terminal 0 (empty = use GUI window)
- `terminal0_baud_rate`: Baud rate (300, 1200, 2400, 4800, 9600, 19200)
- `terminal0_flow_control`: Hardware flow control (0=off, 1=on)

Repeat for terminals 1, 2, and 3 as needed.

### Step 2: Physical Connection

1. Connect your physical Wang 2236 terminal (or compatible) to the specified COM port
2. Use a null-modem cable if needed
3. Configure the physical terminal to match the baud rate and settings

### Step 3: Terminal Settings

Physical terminals should be configured with:
- Baud rate: 19200 (or as configured)
- Data bits: 8
- Parity: Odd (Wang standard)
- Stop bits: 1
- Flow control: As configured

## Example Setup

With the configuration above:
- Terminal 0 will connect to COM3 (physical terminal)
- Terminal 1 will use the standard GUI window
- The emulated Wang 2200 system will see both terminals as MXD terminals

## Troubleshooting

1. **COM port fails to open**: Check that the port exists and isn't in use
2. **No communication**: Verify baud rate, parity, and cable wiring
3. **Character corruption**: Check parity settings (Wang uses odd parity)
4. **Fallback behavior**: If COM port fails to open, terminal falls back to GUI mode

## Technical Details

- The implementation extends the existing `IoCardTermMux` (MXD card)
- Each terminal can independently be configured for COM port or GUI
- Serial communication uses overlapped I/O on Windows
- Terminal protocol remains standard Wang 2236/MXD