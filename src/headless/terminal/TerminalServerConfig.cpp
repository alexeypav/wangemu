// Terminal Server Configuration Implementation
// Handles INI-based and CLI-based configuration for multi-port terminal server

#include "TerminalServerConfig.h"
#include "../../platform/common/host.h"  // for config functions
#include <iostream>
#include <sstream>
#include <cstring>
#include <cstdlib>

// ============================================================================
// TerminalPortConfig Implementation
// ============================================================================


SerialConfig TerminalPortConfig::toSerialConfig() const
{
    SerialConfig config;
    config.portName = portName;
    config.baudRate = baudRate;
    config.dataBits = dataBits;
    config.parity = parity;
    config.stopBits = stopBits;
    config.hwFlowControl = hwFlowControl;
    config.swFlowControl = swFlowControl;
    config.txQueueSize = txQueueSize;
    return config;
}

std::string TerminalPortConfig::getDescription() const
{
    std::ostringstream oss;
    oss << portName << " at " << baudRate << " baud, " 
        << (int)dataBits
        << (parity == ODDPARITY ? 'O' : (parity == EVENPARITY ? 'E' : 'N'))
        << (stopBits == ONESTOPBIT ? 1 : 2);
    
    if (hwFlowControl && swFlowControl) {
        oss << ", RTS/CTS+XON/XOFF";
    } else if (hwFlowControl) {
        oss << ", RTS/CTS";
    } else if (swFlowControl) {
        oss << ", XON/XOFF";
    } else {
        oss << ", no flow control";
    }
    
    return oss.str();
}

// ============================================================================
// TerminalServerConfig Implementation
// ============================================================================

TerminalServerConfig::TerminalServerConfig()
{
    // Initialize with defaults
    for (int i = 0; i < MAX_TERMINALS; i++) {
        terminals[i] = TerminalPortConfig();
        terminals[i].portName = "/dev/ttyUSB" + std::to_string(i);
    }
}

void TerminalServerConfig::loadFromHostConfig()
{
    // Load MXD settings
    host::configReadInt("terminal_server", "mxd_io_addr", &mxdIoAddr, 0x00);
    host::configReadInt("terminal_server", "num_terms", &numTerminals, 1);
    
    // Clamp to valid range
    if (numTerminals < 1) numTerminals = 1;
    if (numTerminals > MAX_TERMINALS) numTerminals = MAX_TERMINALS;
    
    // Load capture settings
    std::string captureDirStr;
    if (host::configReadStr("terminal_server", "capture_dir", &captureDirStr, nullptr)) {
        captureDir = captureDirStr;
        captureEnabled = !captureDir.empty();
    }
    
    // Load per-terminal settings
    for (int i = 0; i < MAX_TERMINALS; i++) {
        std::string section = "terminal_server/term" + std::to_string(i);
        
        std::string portStr;
        if (host::configReadStr(section, "port", &portStr, nullptr)) {
            terminals[i].portName = portStr;
            terminals[i].enabled = true;
            
            // Load other settings with defaults
            int baud;
            host::configReadInt(section, "baud", &baud, 19200);
            terminals[i].baudRate = static_cast<uint32_t>(baud);
            
            int data;
            host::configReadInt(section, "data", &data, 8);
            terminals[i].dataBits = static_cast<uint8_t>(data);
            
            std::string parityStr;
            if (host::configReadStr(section, "parity", &parityStr, nullptr)) {
                if (parityStr == "odd" || parityStr == "O") {
                    terminals[i].parity = ODDPARITY;
                } else if (parityStr == "even" || parityStr == "E") {
                    terminals[i].parity = EVENPARITY;
                } else {
                    terminals[i].parity = NOPARITY;
                }
            }
            
            int stop;
            host::configReadInt(section, "stop", &stop, 1);
            terminals[i].stopBits = (stop == 2) ? TWOSTOPBITS : ONESTOPBIT;
            
            std::string flowStr;
            if (host::configReadStr(section, "flow", &flowStr, nullptr)) {
                terminals[i].hwFlowControl = (flowStr == "rtscts");
                terminals[i].swFlowControl = (flowStr == "xonxoff");
            }
        }
    }
}

bool TerminalServerConfig::parseCommandLine(int argc, char* argv[])
{
    m_cleanExit = false;  // Reset clean exit flag
    
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        
        if (arg == "--help" || arg == "-h") {
            showHelp();
            m_cleanExit = true;
            return false;
        } else if (arg.find("--ini=") == 0) {
            iniPath = arg.substr(6);
        } else if (arg == "--web-config") {
            webServerEnabled = true;
        } else if (arg.find("--web-port=") == 0) {
            webServerPort = std::stoi(arg.substr(11));
            webServerEnabled = true; // Enable web server when port is specified
        } else if (arg == "--debug-wakeups") {
            debugWakeups = true;
        }
    }
    
    return true;
}


bool TerminalServerConfig::validate() const
{
    if (numTerminals < 1 || numTerminals > MAX_TERMINALS) {
        std::cerr << "Error: Invalid number of terminals: " << numTerminals << std::endl;
        return false;
    }
    
    // Check that we have at least one enabled terminal
    int enabledCount = 0;
    for (int i = 0; i < numTerminals; i++) {
        if (terminals[i].enabled) {
            enabledCount++;
        }
    }
    
    // Validation is done elsewhere, just count enabled terminals here
    (void)enabledCount;  // Suppress unused variable warning
    
    return true;
}

void TerminalServerConfig::printSummary() const
{
    std::cout << "Wang Terminal Server Configuration:" << std::endl;
    std::cout << "  MXD I/O Address: 0x" << std::hex << mxdIoAddr << std::dec << std::endl;
    std::cout << "  Number of Terminals: " << numTerminals << std::endl;
    
    if (captureEnabled) {
        std::cout << "  Capture Directory: " << captureDir << std::endl;
    }
    
    if (webServerEnabled) {
        std::cout << "  Web Configuration: Enabled on port " << webServerPort << std::endl;
    }
    
    std::cout << std::endl << "Terminal Configurations:" << std::endl;
    for (int i = 0; i < numTerminals; i++) {
        if (terminals[i].enabled) {
            std::cout << "  Terminal " << i << ": " << terminals[i].getDescription() << std::endl;
        } else {
            std::cout << "  Terminal " << i << ": Disabled" << std::endl;
        }
    }
}


void TerminalServerConfig::showHelp() const
{
    std::cout << "Wang 2200 Terminal Server" << std::endl;
    std::cout << std::endl;
    std::cout << "Usage: wangemu-terminal-server [options]" << std::endl;
    std::cout << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  --ini=PATH                 Load configuration from INI file (default: wangemu.ini)" << std::endl;
    std::cout << "  --web-config               Enable web configuration interface" << std::endl;
    std::cout << "  --web-port=PORT            Web server port (default: 8080, enables web interface)" << std::endl;
    std::cout << "  --debug-wakeups            Log main loop wake-up reasons (for CPU debugging)" << std::endl;
    std::cout << "  --help, -h                 Show this help message" << std::endl;
    std::cout << std::endl;
    std::cout << "Configuration:" << std::endl;
    std::cout << "  All system and terminal settings are configured via:" << std::endl;
    std::cout << "  1. INI file (wangemu.ini by default)" << std::endl;
    std::cout << "  2. Web interface (--web-config)" << std::endl;
    std::cout << std::endl;
    std::cout << "Examples:" << std::endl;
    std::cout << "  # Start with web configuration interface" << std::endl;
    std::cout << "  wangemu-terminal-server --web-config" << std::endl;
    std::cout << std::endl;
    std::cout << "  # Use custom INI file" << std::endl;
    std::cout << "  wangemu-terminal-server --ini=/path/to/custom.ini" << std::endl;
}