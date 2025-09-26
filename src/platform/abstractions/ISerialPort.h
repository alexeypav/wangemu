#ifndef _INCLUDE_ISERIAL_PORT_H_
#define _INCLUDE_ISERIAL_PORT_H_

#include <string>
#include <memory>

// Forward declarations
class Timer;

/**
 * Abstract interface for serial port implementations.
 * This allows platform-specific implementations while keeping
 * the core terminal mux code platform-independent.
 */
class ISerialPort {
public:
    // Serial port configuration
    enum Parity {
        NOPARITY = 0,
        ODDPARITY = 1,
        EVENPARITY = 2
    };

    enum StopBits {
        ONESTOPBIT = 0,
        TWOSTOPBITS = 2
    };

    struct Config {
        int baudRate = 19200;
        int dataBits = 8;
        Parity parity = ODDPARITY;
        StopBits stopBits = ONESTOPBIT;
        bool enableXonXoff = true;  // Software flow control
        bool enableHardwareFlowControl = false;
    };

    virtual ~ISerialPort() = default;

    /**
     * Open the serial port with the specified configuration
     * @param portName Platform-specific port name (e.g., "COM1" on Windows, "/dev/ttyUSB0" on Linux)
     * @param config Serial port configuration
     * @return true if successful, false otherwise
     */
    virtual bool open(const std::string& portName, const Config& config) = 0;

    /**
     * Close the serial port
     */
    virtual void close() = 0;

    /**
     * Check if the port is open
     * @return true if open, false otherwise
     */
    virtual bool isOpen() const = 0;

    /**
     * Write data to the serial port
     * @param data Pointer to data buffer
     * @param length Number of bytes to write
     * @return Number of bytes actually written, -1 on error
     */
    virtual int write(const void* data, int length) = 0;

    /**
     * Read data from the serial port (non-blocking)
     * @param buffer Buffer to receive data
     * @param bufferSize Maximum number of bytes to read
     * @return Number of bytes actually read, -1 on error, 0 if no data available
     */
    virtual int read(void* buffer, int bufferSize) = 0;

    /**
     * Check how many bytes are available for reading
     * @return Number of bytes available, -1 on error
     */
    virtual int bytesAvailable() const = 0;

    /**
     * Flush output buffers
     */
    virtual void flush() = 0;

    /**
     * Get last error message
     * @return Human-readable error description
     */
    virtual std::string getLastError() const = 0;

    /**
     * Get the port name that was used to open this port
     * @return Port name string
     */
    virtual std::string getPortName() const = 0;

    /**
     * Set receive timeout for read operations
     * @param timeoutMs Timeout in milliseconds, 0 for non-blocking
     */
    virtual void setReadTimeout(int timeoutMs) = 0;
};

/**
 * Factory function to create platform-specific serial port implementation
 * @return Unique pointer to serial port implementation
 */
std::unique_ptr<ISerialPort> createSerialPort();

/**
 * Get list of available serial ports on the system
 * @return Vector of port names
 */
std::vector<std::string> getAvailableSerialPorts();

#endif // _INCLUDE_ISERIAL_PORT_H_