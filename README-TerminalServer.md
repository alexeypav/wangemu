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
# Clone and build x86_64 version
git clone <repository-url>
cd wangemu
make -f makefile.terminal-server

# Or build ARM64 version for Raspberry Pi
make -f makefile.terminal-server-aarch64

# The binary will be created as 'wangemu-terminal-server' or 'wangemu-terminal-server-aarch64'
```

### Configuration-Based Setup

The terminal server uses **configuration-only** approach - no more command line terminal configuration:

```bash
# Enable web configuration interface
./wangemu-terminal-server --web-config

# Use custom INI file
./wangemu-terminal-server --ini=/path/to/custom.ini

# Run with defaults (uses wangemu.ini in current directory)
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

The terminal server uses a **simplified command line interface** - all system and terminal settings are configured via INI files or web interface:

```
Usage: wangemu-terminal-server [options]

Options:
  --ini=PATH                 Load configuration from INI file (default: wangemu.ini)
  --web-config               Enable web configuration interface
  --web-port=PORT            Web server port (default: 8080, enables web interface)
  --help, -h                 Show this help message
```

### Configuration Sources

All system and terminal settings are configured via:
1. **INI file** (wangemu.ini by default) 
2. **Web interface** (--web-config)

**Examples:**
- `./wangemu-terminal-server --web-config` - Start with web configuration interface
- `./wangemu-terminal-server --ini=/path/to/custom.ini` - Use custom INI file
- `./wangemu-terminal-server --web-port=9090` - Web interface on port 9090

### INI File Configuration

The terminal server uses a **modern simplified INI format** with separate sections for cleaner organization:

```ini
[terminal_server]
mxd_io_addr=0x00
num_terms=2
capture_dir=/var/log/wangemu

[terminal_server/term0]
port=/dev/ttyUSB0
baud=19200
data=8
parity=odd
stop=1
flow=xonxoff

[terminal_server/term1]  
port=/dev/ttyUSB1
baud=19200
data=8
parity=odd
stop=1
flow=xonxoff
```

**Available Settings:**
- `port`: Serial device path (`/dev/ttyUSB0`, `/dev/ttyACM0`, etc.)
- `baud`: Baud rate (300, 1200, 2400, 4800, 9600, 19200, 38400, etc.)
- `data`: Data bits (7 or 8)
- `parity`: Parity (none, odd, even)  
- `stop`: Stop bits (1 or 2)
- `flow`: Flow control (none, xonxoff, rtscts)

## Usage Examples

### Web Configuration

```bash
# Start with web interface (recommended)
./wangemu-terminal-server --web-config

# Access web interface at http://localhost:8080
# Configure terminals through the web UI
```

### INI File Configuration

```bash
# Create custom configuration
./wangemu-terminal-server --ini=/etc/wangemu-server.ini

# Run with default configuration
./wangemu-terminal-server
```

### Development & Testing

```bash
# Start with web interface for testing
./wangemu-terminal-server --web-config --web-port=9090

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


## Null modem wiring

## 9-Wire Map (DB25 ↔ DB9)

| DB9 (PC/Emu) | DB25 (Wang 2336DW) | Signal     | Notes                                     |
|--------------|--------------------|------------|-------------------------------------------|
| 1            | 20                 | DTR → DCD  | Tie Terminal Ready to Host Carrier Detect |
| 2            | 2                  | TXD → RXD  | Terminal TX → Host RX                     |
| 3            | 3                  | RXD ← TXD  | Terminal RX ← Host TX                     |
| 4            | 6, 8               | DSR/DCD ← DTR | Host Ready & Carrier Detect → Terminal  |
| 5            | 7                  | GND        | Common ground                             |
| 6            | 20                 | DTR → DSR  | Terminal Ready → Host                     |
| 7            | 5                  | CTS ↔ RTS  | Crossed handshaking                       |
| 8            | 4                  | RTS ↔ CTS  | Crossed handshaking                       |

## Related Documentation

- [Main README](README.md) - GUI emulator build and usage
- [Original Wang 2200 Documentation](http://www.wang2200.org) - Hardware reference
- [Terminal Wiring Guide](http://www.wang2200.org/terminals.html) - Cable specifications
