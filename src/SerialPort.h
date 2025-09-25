#ifndef _INCLUDE_SERIAL_PORT_H_
#define _INCLUDE_SERIAL_PORT_H_

#include "w2200.h"
#include "Scheduler.h"

// Forward declarations
class Timer;

#ifdef _WIN32
#include <windows.h>
// Undefine Windows macros that conflict with our enums
#ifdef NOPARITY
#undef NOPARITY
#endif
#ifdef ODDPARITY
#undef ODDPARITY
#endif
#ifdef EVENPARITY
#undef EVENPARITY
#endif
#ifdef ONESTOPBIT
#undef ONESTOPBIT
#endif
#ifdef TWOSTOPBITS
#undef TWOSTOPBITS
#endif
#endif

#include <string>
#include <memory>
#include <functional>
#include <queue>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>

class Terminal;
class Scheduler;

// Platform-agnostic parity constants
enum ParityType {
    NOPARITY = 0,
    ODDPARITY = 1,
    EVENPARITY = 2
};

// Platform-agnostic stop bits constants  
enum StopBitsType {
    ONESTOPBIT = 0,
    TWOSTOPBITS = 1
};

// Wang 2200 serial port settings commonly used
struct SerialConfig {
    std::string portName;        // e.g. "COM1", "/dev/ttyUSB0"
    uint32_t baudRate;          // 300, 1200, 2400, 4800, 9600, 19200
    uint8_t dataBits;           // 7 or 8
    StopBitsType stopBits;      // ONESTOPBIT, TWOSTOPBITS  
    ParityType parity;          // NOPARITY, ODDPARITY, EVENPARITY
    bool hwFlowControl;         // Hardware flow control (RTS/CTS) - not used for Wang terminals
    bool swFlowControl;         // Software flow control (XON/XOFF) - recommended for Wang terminals
    
    SerialConfig() :
#ifdef _WIN32
        portName("COM5"),
#else
        portName("/dev/ttyUSB0"),
#endif
        baudRate(19200),
        dataBits(8), 
        stopBits(ONESTOPBIT),
        parity(ODDPARITY),
        hwFlowControl(false),   // Disable hardware flow control by default
        swFlowControl(false)    // Disable software flow control by default
    {}
};

class SerialPort
{
public:
    CANT_ASSIGN_OR_COPY_CLASS(SerialPort);

    SerialPort(std::shared_ptr<Scheduler> scheduler);
    ~SerialPort();

    // Configuration
    bool open(const SerialConfig &config);
    void close();
    bool isOpen() const;

    // Terminal connection
    void attachTerminal(std::shared_ptr<Terminal> terminal);
    void detachTerminal();

    // Data transmission (called by Terminal when user types)
    void sendByte(uint8 byte);
    void sendData(const uint8 *data, size_t length);
    
    // Receive callback for MXD integration
    using RxCallback = std::function<void(uint8)>;
    void setReceiveCallback(RxCallback cb);
    
    // Statistics
    uint64_t getRxByteCount() const { return m_rxByteCount.load(); }
    uint64_t getTxByteCount() const { return m_txByteCount.load(); }
    void resetCounters() { m_rxByteCount.store(0); m_txByteCount.store(0); }
    
    // Connection status
    bool isConnected() const { return m_connected.load(); }
    int getReconnectAttempts() const { return m_reconnectAttempts.load(); }
    
    // Capture hooks for debugging
    using CaptureCallback = std::function<void(uint8, bool)>;  // byte, isRx
    void setCaptureCallback(CaptureCallback cb) { m_captureCallback = std::move(cb); }

private:
    // Internal communication methods
    void startReceiving();
    void stopReceiving();
    void receiveThreadProc();
    void processReceivedByte(uint8 byte);

    // Serial port transmission with timing
    void transmitByte(uint8 byte);
    void onTransmitComplete();

    std::shared_ptr<Scheduler> m_scheduler;
    std::shared_ptr<Terminal> m_terminal;
    
    // MXD receive callback
    RxCallback m_rxCallback;
    
    // Capture callback for debugging
    CaptureCallback m_captureCallback;

#ifdef _WIN32
    HANDLE m_handle;
    OVERLAPPED m_readOverlapped;
    OVERLAPPED m_writeOverlapped;
    HANDLE m_readEvent;
    HANDLE m_writeEvent;
#else
    int m_fd;                   // POSIX file descriptor
    int m_cancelPipe[2];        // pipe for thread cancellation
#endif

    // Receiving thread
    std::thread m_receiveThread;
    std::atomic<bool> m_stopReceiving;

    // Transmit queue and timing (model serial UART delays)
    std::queue<uint8> m_txQueue;
    std::recursive_mutex m_txMutex;
    std::shared_ptr<Timer> m_txTimer;
    bool m_txBusy;

    // Configuration
    SerialConfig m_config;
    
    // Statistics counters (thread-safe)
    std::atomic<uint64_t> m_rxByteCount{0};
    std::atomic<uint64_t> m_txByteCount{0};
    
    // Reconnection state
    std::atomic<bool> m_connected{false};
    std::atomic<int> m_reconnectAttempts{0};
    std::chrono::steady_clock::time_point m_lastReconnectAttempt;
    static constexpr int MAX_RECONNECT_ATTEMPTS = 10;
    static constexpr int BASE_RECONNECT_DELAY_MS = 250;
    
    // Internal methods
    bool attemptReconnect();
    int getReconnectDelayMs() const;

    // Calculate transmission delay based on baud rate and settings
    int64 calculateTransmitDelay() const;
};

#endif // _INCLUDE_SERIAL_PORT_H_