// Terminal Server Configuration Implementation
// Handles INI-based and CLI-based configuration for headless multi-port terminal server

#include "TerminalServerConfig.h"
#include "host.h"  // for config functions
#include <iostream>
#include <sstream>
#include <cstring>
#include <cstdlib>

// ============================================================================
// TerminalPortConfig Implementation
// ============================================================================

bool TerminalPortConfig::parseFromString(const std::string& spec)
{
    // Parse format: "/dev/ttyUSB0,19200,8,O,1[,xonxoff]"
    std::istringstream ss(spec);
    std::string token;
    std::vector<std::string> parts;
    
    while (std::getline(ss, token, ',')) {
        parts.push_back(token);
    }
    
    if (parts.size() < 5) {
        std::cerr << "Error: Terminal spec requires at least 5 parts: port,baud,data,parity,stop" << std::endl;
        return false;
    }
    
    // Parse port name
    portName = parts[0];
    
    // Parse baud rate
    baudRate = static_cast<uint32_t>(std::stoul(parts[1]));
    
    // Parse data bits
    dataBits = static_cast<uint8_t>(std::stoul(parts[2]));
    if (dataBits != 7 && dataBits != 8) {
        std::cerr << "Error: Data bits must be 7 or 8" << std::endl;
        return false;
    }
    
    // Parse parity
    if (parts[3] == "N" || parts[3] == "n") {
        parity = NOPARITY;
    } else if (parts[3] == "O" || parts[3] == "o") {
        parity = ODDPARITY;
    } else if (parts[3] == "E" || parts[3] == "e") {
        parity = EVENPARITY;
    } else {
        std::cerr << "Error: Parity must be N, O, or E" << std::endl;
        return false;
    }
    
    // Parse stop bits
    int stopBitsInt = std::stoi(parts[4]);
    if (stopBitsInt == 1) {
        stopBits = ONESTOPBIT;
    } else if (stopBitsInt == 2) {
        stopBits = TWOSTOPBITS;
    } else {
        std::cerr << "Error: Stop bits must be 1 or 2" << std::endl;
        return false;
    }
    
    // Parse optional flow control
    if (parts.size() >= 6) {
        if (parts[5] == "xonxoff" || parts[5] == "XONXOFF") {
            swFlowControl = true;
        } else if (parts[5] == "rtscts" || parts[5] == "RTSCTS") {
            hwFlowControl = true;
        } else if (parts[5] == "none" || parts[5] == "NONE") {
            hwFlowControl = false;
            swFlowControl = false;
        }
    }
    
    enabled = true;
    return true;
}

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
    host::configReadInt("terminal_server", "mxd_io_addr", &mxdIoAddr, 0x46);
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
    bool terminalServerMode = false;
    bool showStatus = false;
    m_cleanExit = false;  // Reset clean exit flag
    
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        
        if (arg == "--help" || arg == "-h") {
            showHelp();
            m_cleanExit = true;
            return false;
        } else if (arg == "--terminal-server") {
            terminalServerMode = true;
        } else if (arg == "--status") {
            showStatus = true;
        } else if (arg.find("--term") == 0) {
            if (!parseTerminalArg(arg)) {
                return false;
            }
        } else if (arg.find("--capture-dir=") == 0) {
            captureDir = arg.substr(14);
            captureEnabled = !captureDir.empty();
        } else if (arg.find("--mxd-addr=") == 0) {
            mxdIoAddr = std::stoi(arg.substr(11), nullptr, 0);
        } else if (arg.find("--num-terms=") == 0) {
            numTerminals = std::stoi(arg.substr(12));
            if (numTerminals < 1) numTerminals = 1;
            if (numTerminals > MAX_TERMINALS) numTerminals = MAX_TERMINALS;
        }
    }
    
    if (showStatus) {
        std::cout << getStatusJson() << std::endl;
        m_cleanExit = true;
        return false;  // Exit after showing status
    }
    
    if (!terminalServerMode) {
        std::cerr << "Error: --terminal-server flag required for headless terminal server mode" << std::endl;
        showHelp();
        return false;
    }
    
    return true;
}

bool TerminalServerConfig::parseTerminalArg(const std::string& arg)
{
    // Parse --termN=spec format
    size_t eqPos = arg.find('=');
    if (eqPos == std::string::npos) {
        std::cerr << "Error: Terminal argument missing '=': " << arg << std::endl;
        return false;
    }
    
    std::string termPart = arg.substr(0, eqPos);
    std::string spec = arg.substr(eqPos + 1);
    
    // Extract terminal number
    if (termPart.length() < 7 || termPart.substr(0, 6) != "--term") {
        std::cerr << "Error: Invalid terminal argument: " << arg << std::endl;
        return false;
    }
    
    int termNum = std::stoi(termPart.substr(6));
    if (termNum < 0 || termNum >= MAX_TERMINALS) {
        std::cerr << "Error: Terminal number out of range (0-" << (MAX_TERMINALS-1) << "): " << termNum << std::endl;
        return false;
    }
    
    // Parse the specification
    if (!terminals[termNum].parseFromString(spec)) {
        std::cerr << "Error: Failed to parse terminal " << termNum << " specification: " << spec << std::endl;
        return false;
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
    
    if (enabledCount == 0) {
        std::cerr << "Error: No terminals configured" << std::endl;
        return false;
    }
    
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
    
    std::cout << std::endl << "Terminal Configurations:" << std::endl;
    for (int i = 0; i < numTerminals; i++) {
        if (terminals[i].enabled) {
            std::cout << "  Terminal " << i << ": " << terminals[i].getDescription() << std::endl;
        } else {
            std::cout << "  Terminal " << i << ": Disabled" << std::endl;
        }
    }
}

std::string TerminalServerConfig::getStatusJson() const
{
    std::ostringstream json;
    json << "{" << std::endl;
    json << "  \"mxd_addr\":\"0x" << std::hex << mxdIoAddr << std::dec << "\"," << std::endl;
    json << "  \"num_terms\":" << numTerminals << "," << std::endl;
    json << "  \"capture_enabled\":" << (captureEnabled ? "true" : "false") << "," << std::endl;
    if (captureEnabled) {
        json << "  \"capture_dir\":\"" << captureDir << "\"," << std::endl;
    }
    json << "  \"terms\":[" << std::endl;
    
    for (int i = 0; i < MAX_TERMINALS; i++) {
        if (i > 0) json << "," << std::endl;
        json << "    {\"id\":" << i << ",";
        json << "\"enabled\":" << (terminals[i].enabled ? "true" : "false");
        if (terminals[i].enabled) {
            json << ",\"port\":\"" << terminals[i].portName << "\"";
            json << ",\"baud\":" << terminals[i].baudRate;
            json << ",\"parity\":\"" << (terminals[i].parity == ODDPARITY ? "O" : 
                                        (terminals[i].parity == EVENPARITY ? "E" : "N")) << "\"";
            json << ",\"xonxoff\":" << (terminals[i].swFlowControl ? "true" : "false");
        }
        json << "}";
    }
    
    json << std::endl << "  ]" << std::endl;
    json << "}" << std::endl;
    return json.str();
}

void TerminalServerConfig::showHelp() const
{
    std::cout << "Wang 2200 Headless Terminal Server" << std::endl;
    std::cout << std::endl;
    std::cout << "Usage: wangemu-headless --terminal-server [options]" << std::endl;
    std::cout << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  --terminal-server          Enable terminal server mode" << std::endl;
    std::cout << "  --termN=PORT,BAUD,DATA,PARITY,STOP[,FLOW]" << std::endl;
    std::cout << "                             Configure terminal N (0-3)" << std::endl;
    std::cout << "                             PORT: /dev/ttyUSB0, /dev/ttyACM0, etc." << std::endl;
    std::cout << "                             BAUD: 300, 1200, 2400, 4800, 9600, 19200, etc." << std::endl;
    std::cout << "                             DATA: 7 or 8" << std::endl;
    std::cout << "                             PARITY: N (none), O (odd), E (even)" << std::endl;
    std::cout << "                             STOP: 1 or 2" << std::endl;
    std::cout << "                             FLOW: none, xonxoff, rtscts (optional)" << std::endl;
    std::cout << "  --mxd-addr=ADDR            MXD I/O address (default: 0x46)" << std::endl;
    std::cout << "  --num-terms=N              Number of terminals (1-4, default: 1)" << std::endl;
    std::cout << "  --capture-dir=DIR          Directory for capture files" << std::endl;
    std::cout << "  --status                   Print status JSON and exit" << std::endl;
    std::cout << "  --help, -h                 Show this help message" << std::endl;
    std::cout << std::endl;
    std::cout << "Examples:" << std::endl;
    std::cout << "  # Single terminal on USB serial adapter" << std::endl;
    std::cout << "  wangemu-headless --terminal-server --term0=/dev/ttyUSB0,19200,8,O,1,xonxoff" << std::endl;
    std::cout << std::endl;
    std::cout << "  # Multiple terminals" << std::endl;
    std::cout << "  wangemu-headless --terminal-server --num-terms=2 \\" << std::endl;
    std::cout << "    --term0=/dev/ttyUSB0,19200,8,O,1,xonxoff \\" << std::endl;
    std::cout << "    --term1=/dev/ttyUSB1,19200,8,O,1,xonxoff" << std::endl;
}