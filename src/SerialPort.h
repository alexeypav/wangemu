#ifndef _INCLUDE_SERIAL_PORT_H_
#define _INCLUDE_SERIAL_PORT_H_

#include "w2200.h"
#include "Scheduler.h"

// Forward declarations
class Timer;

#ifdef _WIN32
#include <windows.h>
#endif

#include <string>
#include <memory>
#include <functional>
#include <queue>
#include <thread>
#include <atomic>
#include <mutex>

class Terminal;
class Scheduler;

// Wang 2200 serial port settings commonly used
struct SerialConfig {
    std::string portName;        // e.g. "COM1", "COM2"
    DWORD baudRate;             // 300, 1200, 2400, 4800, 9600, 19200
    BYTE dataBits;              // 7 or 8
    BYTE stopBits;              // ONESTOPBIT, TWOSTOPBITS  
    BYTE parity;                // NOPARITY, ODDPARITY, EVENPARITY
    bool hwFlowControl;         // Hardware flow control (RTS/CTS) - not used for Wang terminals
    bool swFlowControl;         // Software flow control (XON/XOFF) - recommended for Wang terminals
    
    SerialConfig() :
        portName("COM5"),
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
    bool isOpen() const { return m_handle != INVALID_HANDLE_VALUE; }

    // Terminal connection
    void attachTerminal(std::shared_ptr<Terminal> terminal);
    void detachTerminal();

    // Data transmission (called by Terminal when user types)
    void sendByte(uint8 byte);
    void sendData(const uint8 *data, size_t length);
    
    // Receive callback for MXD integration
    using RxCallback = std::function<void(uint8)>;
    void setReceiveCallback(RxCallback cb);

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

#ifdef _WIN32
    HANDLE m_handle;
    OVERLAPPED m_readOverlapped;
    OVERLAPPED m_writeOverlapped;
    HANDLE m_readEvent;
    HANDLE m_writeEvent;
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

    // Calculate transmission delay based on baud rate and settings
    int64 calculateTransmitDelay() const;
};

#endif // _INCLUDE_SERIAL_PORT_H_