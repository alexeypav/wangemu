# WangEmu 2200 Terminal Server

Modified version of wangemu, to run on Linux and connect multiple physical Wang 2X36 terminals via USB-serial adapters.

## Notes

- Runs without wxWidgets GUI dependencies on Linux
- **Serial Settings**: 19200 baud, 8 data bits, odd parity, 1 stop bit - default for the Wang terminals
- **Flow Control**: XON/XOFF software flow control
- **Adapters**: USB-to-serial adapters appearing as `/dev/ttyUSB*` or `/dev/ttyACM*`

## Quick Start

### Build the Terminal Server

```bash
# Clone and build
git clone <repository-url>
cd wangemu
make -f makefile.terminal-server

# The binary will be created as 'wangemu-terminal-server'
```

### Single Terminal Setup

```bash
# Connect one Wang terminal
./wangemu-terminal-server --term0=/dev/ttyUSB0,19200,8,O,1,xonxoff
```

### Multiple Terminals

```bash
# Connect multiple terminals
./wangemu-terminal-server --num-terms=2 \
  --term0=/dev/ttyUSB0,19200,8,O,1,xonxoff \
  --term1=/dev/ttyUSB1,19200,8,O,1,xonxoff
```

### Default Configuration

```bash
# Run with default settings (terminal 0 on /dev/ttyUSB0)
./wangemu-terminal-server
```

## Building

### Prerequisites

- Linux system (WSL2 supported)
- g++ compiler with C++14 support
- pthread library
- No wxWidgets required

### Build Process

```bash
# Build the terminal server
make -f makefile.terminal-server

# For optimized build
make -f makefile.terminal-server opt

# Clean build artifacts
make -f makefile.terminal-server clean
```

The terminal server build excludes all GUI components and dependencies, creating a lightweight (~4MB) binary optimized for server deployment.

### Cross-Compilation for ARM64/aarch64

To build for ARM64 systems (like Raspberry Pi 4+, ARM-based servers):

```bash
# Install cross-compilation toolchain (Ubuntu/Debian)
sudo apt update
sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu

# Build static ARM64 binary
make -f makefile.terminal-server-aarch64 clean
make -f makefile.terminal-server-aarch64 opt

# Output: wangemu-terminal-server-aarch64 (fully static, ~3.4MB)
```


## Configuration

### Auto Start
  1. start-wangemu.sh - Starts the terminal server with --web-config argument
  2. setup-startup.sh - Configures the system to run the startup script on boot

  On your Raspberry Pi, run:
  sudo ./setup-startup.sh

  This will add the startup script to /etc/rc.local so it runs automatically on boot.

### Command Line Options

```
Usage: wangemu-terminal-server [options]

Options:
  --termN=PORT,BAUD,DATA,PARITY,STOP[,FLOW]
                             Configure terminal N (0-3)
                             PORT: /dev/ttyUSB0, /dev/ttyACM0, etc.
                             BAUD: 300, 1200, 2400, 4800, 9600, 19200, etc.
                             DATA: 7 or 8
                             PARITY: N (none), O (odd), E (even)
                             STOP: 1 or 2
                             FLOW: none, xonxoff, rtscts (optional)
  --mxd-addr=ADDR            MXD I/O address (default: 0x00)
  --num-terms=N              Number of terminals (1-4, default: 1)
  --capture-dir=DIR          Directory for capture files
  --ini=PATH                 Load configuration from INI file
  --status                   Print status JSON and exit
  --help, -h                 Show this help message
```

### Terminal Configuration Format

```
--termN=PORT,BAUD,DATA,PARITY,STOP[,FLOW]
```

**Examples:**
- `--term0=/dev/ttyUSB0,19200,8,O,1,xonxoff` - Standard Wang terminal
- `--term1=/dev/ttyACM0,9600,7,E,2,none` - Custom configuration
- `--term2=/dev/ttyUSB2,19200,8,N,1` - No flow control

### INI File Configuration

The terminal server uses the same `wangemu.ini` format as the GUI version:

```ini
[wangemu/config-0/cpu]
cpu=2200MVP-C
memsize=512

[wangemu/config-0/io/slot-0]
addr=0x000
type=2236 MXD

[wangemu/config-0/io/slot-0/cardcfg]
numTerminals=2
terminal0_com_port=/dev/ttyUSB0
terminal0_baud_rate=19200
terminal0_sw_flow_control=1
terminal1_com_port=/dev/ttyUSB1
terminal1_baud_rate=19200
terminal1_sw_flow_control=1
```

## Usage Examples

## Multiple Terminals

```bash
# Run as service with multiple terminals
./wangemu-terminal-server --num-terms=4 \
  --term0=/dev/ttyUSB0,19200,8,O,1,xonxoff \
  --term1=/dev/ttyUSB1,19200,8,O,1,xonxoff \
  --term2=/dev/ttyUSB2,19200,8,O,1,xonxoff \
  --term3=/dev/ttyUSB3,19200,8,O,1,xonxoff \
  --capture-dir=/var/log/wangemu
```

### Development & Testing

```bash
# Test with null device (no hardware needed)
./wangemu-terminal-server --term0=/dev/null,19200,8,O,1,none

# Check configuration
./wangemu-terminal-server --status

# Monitor runtime statistics
kill -SIGUSR1 $(pidof wangemu-terminal-server)
```

## Troubleshooting

### Permission Issues

```bash
# Add user to dialout group for serial port access
sudo usermod -a -G dialout $USER

# Or use udev rules for specific devices
echo 'KERNEL=="ttyUSB*", GROUP="dialout", MODE="0666"' | sudo tee /etc/udev/rules.d/50-wang-terminals.rules
sudo udevadm control --reload-rules
```

### Serial Port Detection

```bash
# List available serial ports
ls /dev/ttyUSB* /dev/ttyACM*

# Check port permissions
ls -l /dev/ttyUSB0

# Test port access
stty -F /dev/ttyUSB0 19200 cs8 parenb parodd -cstopb -crtscts ixon ixoff
```

### Common Issues

**"Permission denied" on serial port:**
- Check user permissions (dialout group membership)
- Verify port ownership and mode
- Try running with sudo (not recommended for production)

## Architecture

The terminal server implements session abstraction:

```
┌─────────────────┐    ┌──────────────┐    ┌─────────────────┐
│   Wang 2200     │    │ Serial Term  │    │ Physical Wang   │
│   CPU Emulator  │◄──►│ Session      │◄──►│ Terminal        │
│   (MXD Card)    │    │ Abstraction  │    │ (2236/2336)     │
└─────────────────┘    └──────────────┘    └─────────────────┘
```

- **MXD Terminal Multiplexer**: Emulates Wang's multi-port terminal controller
- **Session Abstraction**: Clean interface between emulated MXD and real terminals
- **POSIX Serial Port**: Full-featured serial communication with proper flow control
- **Configuration Management**: Unified CLI and INI-based configuration system

## Development

### Code Organization

- `src/main_headless.cpp` - Terminal server main entry point
- `src/TerminalServerConfig.*` - Configuration management (CLI + INI)
- `src/SerialTermSession.*` - Terminal session abstraction
- `src/SerialPort.*` - Platform-agnostic serial port implementation
- `src/IoCardTermMux.*` - MXD terminal multiplexer emulation
- `src/UiHeadless.cpp` - Minimal UI implementations for terminal server mode

### Testing

```bash
# Build and test
make -f makefile.terminal-server
./wangemu-terminal-server --help
./wangemu-terminal-server --status

# Test with virtual terminals using socat
socat -d -d pty,raw,echo=0,link=/tmp/vterm0 pty,raw,echo=0,link=/tmp/vterm1 &
./wangemu-terminal-server --term0=/tmp/vterm0,19200,8,O,1,xonxoff

# Connect to virtual terminal - not tested?
screen /tmp/vterm1 19200,cs8,parenb,parodd,-cstopb,-crtscts,ixon,ixoff
```

---

## Related Documentation

- [Main README](README.md) - GUI emulator build and usage
- [Original Wang 2200 Documentation](http://www.wang2200.org) - Hardware reference
- [Terminal Wiring Guide](http://www.wang2200.org/terminals.html) - Cable specifications
