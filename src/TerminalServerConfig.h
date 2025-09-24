#ifndef _INCLUDE_TERMINAL_SERVER_CONFIG_H_
#define _INCLUDE_TERMINAL_SERVER_CONFIG_H_

#include "SerialPort.h"
#include <string>
#include <vector>

/**
 * Configuration for a single terminal in the terminal server
 */
struct TerminalPortConfig {
    std::string portName;           // e.g. "/dev/ttyUSB0"
    uint32_t baudRate;             // e.g. 19200
    uint8_t dataBits;              // 7 or 8
    ParityType parity;             // NOPARITY, ODDPARITY, EVENPARITY
    StopBitsType stopBits;         // ONESTOPBIT, TWOSTOPBITS
    bool hwFlowControl;            // Hardware flow control (RTS/CTS)
    bool swFlowControl;            // Software flow control (XON/XOFF)
    bool enabled;                  // Whether this terminal is enabled
    
    TerminalPortConfig() :
        portName("/dev/ttyUSB0"),
        baudRate(19200),
        dataBits(8),
        parity(ODDPARITY),
        stopBits(ONESTOPBIT),
        hwFlowControl(false),      // Wang terminals don't use hardware flow control
        swFlowControl(true),       // Enable XON/XOFF for Wang terminals
        enabled(false)
    {}
    
    /**
     * Parse a CLI terminal specification string
     * Format: "/dev/ttyUSB0,19200,8,O,1[,xonxoff]"
     */
    bool parseFromString(const std::string& spec);
    
    /**
     * Convert to SerialConfig for SerialPort
     */
    SerialConfig toSerialConfig() const;
    
    /**
     * Get a human-readable description
     */
    std::string getDescription() const;
};

/**
 * Terminal server configuration
 */
class TerminalServerConfig {
public:
    static constexpr int MAX_TERMINALS = 4;
    
    TerminalServerConfig();
    
    // MXD configuration
    int mxdIoAddr = 0x00;              // Default MXD I/O address
    int numTerminals = 1;              // Number of active terminals
    
    // Terminal configurations
    TerminalPortConfig terminals[MAX_TERMINALS];
    
    // Capture settings
    std::string captureDir;            // Directory for capture files (empty = disabled)
    bool captureEnabled = false;       // Global capture enable
    
    // INI file settings
    std::string iniPath;               // Path to INI file to load (empty = default)
    
    /**
     * Load configuration from host config system (INI-style)
     */
    void loadFromHostConfig();
    
    /**
     * Parse command line arguments to override config
     * @param argc Number of arguments
     * @param argv Argument array
     * @return true if parsing succeeded, false on error or clean exit
     */
    bool parseCommandLine(int argc, char* argv[]);
    
    /**
     * Check if last parse was a clean exit (help/status)
     * @return true if should exit with code 0
     */
    bool shouldExitCleanly() const { return m_cleanExit; }

    /**
     * Validate the configuration
     * @return true if valid, false otherwise
     */
    bool validate() const;
    
    /**
     * Print configuration summary
     */
    void printSummary() const;

    bool shouldExit() const { return m_cleanExit; }

private:
    bool m_cleanExit = false;  // Track clean exits for help/status
    
    /**
     * Get status as JSON string for --status command
     */
    std::string getStatusJson() const;

private:
    /**
     * Parse a single terminal specification from command line
     * Format: --termN=/dev/ttyUSBk,baud,data,parity,stop[,xonxoff]
     */
    bool parseTerminalArg(const std::string& arg);
    
    /**
     * Show help message
     */
    void showHelp() const;
};

#endif // _INCLUDE_TERMINAL_SERVER_CONFIG_H_