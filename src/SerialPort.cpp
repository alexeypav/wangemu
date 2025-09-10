// Windows Serial Port implementation for Wang 2236 terminal communication
// Uses overlapped I/O for non-blocking serial communication

#include "SerialPort.h"
#include "Terminal.h"
#include "Scheduler.h"
#include "host.h"  // for dbglog()

#ifdef _WIN32
#include <windows.h>
#include <iostream>
#include <cassert>

SerialPort::SerialPort(std::shared_ptr<Scheduler> scheduler) :
    m_scheduler(scheduler),
    m_handle(INVALID_HANDLE_VALUE),
    m_stopReceiving(false),
    m_txBusy(false)
{
    // Initialize overlapped structures
    memset(&m_readOverlapped, 0, sizeof(m_readOverlapped));
    memset(&m_writeOverlapped, 0, sizeof(m_writeOverlapped));
    
    // Create events for overlapped I/O
    m_readEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    m_writeEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    
    m_readOverlapped.hEvent = m_readEvent;
    m_writeOverlapped.hEvent = m_writeEvent;
}

SerialPort::~SerialPort()
{
    close();
    
    if (m_readEvent != nullptr) {
        CloseHandle(m_readEvent);
        m_readEvent = nullptr;
    }
    
    if (m_writeEvent != nullptr) {
        CloseHandle(m_writeEvent);
        m_writeEvent = nullptr;
    }
}

bool SerialPort::open(const SerialConfig &config)
{
    if (isOpen()) {
        close();
    }
    
    m_config = config;
    
    // Open the COM port
    m_handle = CreateFileA(
        config.portName.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,                      // exclusive access
        nullptr,                // default security
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,   // overlapped I/O
        nullptr
    );
    
    if (m_handle == INVALID_HANDLE_VALUE) {
        dbglog("SerialPort::open() - Failed to open %s, error %d\n", 
               config.portName.c_str(), GetLastError());
        return false;
    }
    
    // Configure the port
    DCB dcb = {0};
    dcb.DCBlength = sizeof(dcb);
    
    if (!GetCommState(m_handle, &dcb)) {
        dbglog("SerialPort::open() - GetCommState failed, error %d\n", GetLastError());
        close();
        return false;
    }
    
    // Set Wang 2200 compatible settings
    dcb.BaudRate = config.baudRate;
    dcb.ByteSize = config.dataBits;
    dcb.StopBits = config.stopBits;
    dcb.Parity = config.parity;
    
    // Flow control settings for Wang hardware
    if (config.flowControl) {
        dcb.fOutxCtsFlow = TRUE;    // CTS output flow control
        dcb.fRtsControl = RTS_CONTROL_HANDSHAKE;  // RTS handshaking
        dcb.fDtrControl = DTR_CONTROL_ENABLE;     // DTR on
        dcb.fDsrSensitivity = TRUE; // DSR sensitivity
    } else {
        dcb.fOutxCtsFlow = FALSE;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;
        dcb.fDsrSensitivity = FALSE;
    }
    
    // Disable software flow control (Wang uses hardware)
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    
    if (!SetCommState(m_handle, &dcb)) {
        dbglog("SerialPort::open() - SetCommState failed, error %d\n", GetLastError());
        close();
        return false;
    }
    
    // Set timeouts for Wang 2200 compatibility
    COMMTIMEOUTS timeouts = {0};
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = 0;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 1000; // 1 second write timeout
    
    if (!SetCommTimeouts(m_handle, &timeouts)) {
        dbglog("SerialPort::open() - SetCommTimeouts failed, error %d\n", GetLastError());
        close();
        return false;
    }
    
    // Purge any existing data
    PurgeComm(m_handle, PURGE_RXCLEAR | PURGE_TXCLEAR);
    
    // Start receiving thread
    startReceiving();
    
    dbglog("SerialPort::open() - Opened %s at %d baud, %d%c%d, flow control %s\n",
           config.portName.c_str(), config.baudRate, config.dataBits,
           (config.parity == NOPARITY) ? 'N' : (config.parity == ODDPARITY) ? 'O' : 'E',
           (config.stopBits == ONESTOPBIT) ? 1 : 2,
           config.flowControl ? "ON" : "OFF");
    
    return true;
}

void SerialPort::close()
{
    if (!isOpen()) {
        return;
    }
    
    stopReceiving();
    
    // Cancel any pending I/O
    CancelIo(m_handle);
    
    CloseHandle(m_handle);
    m_handle = INVALID_HANDLE_VALUE;
    
    // Clear transmit queue
    {
        std::lock_guard<std::mutex> lock(m_txMutex);
        while (!m_txQueue.empty()) {
            m_txQueue.pop();
        }
        m_txBusy = false;
        m_txTimer = nullptr;
    }
    
    dbglog("SerialPort::close() - Closed %s\n", m_config.portName.c_str());
}

void SerialPort::attachTerminal(std::shared_ptr<Terminal> terminal)
{
    m_terminal = terminal;
}

void SerialPort::detachTerminal()
{
    m_terminal = nullptr;
}

void SerialPort::sendByte(uint8 byte)
{
    if (!isOpen()) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(m_txMutex);
    m_txQueue.push(byte);
    
    // Start transmission if not already busy
    if (!m_txBusy && !m_txQueue.empty()) {
        uint8 nextByte = m_txQueue.front();
        m_txQueue.pop();
        transmitByte(nextByte);
    }
}

void SerialPort::sendData(const uint8 *data, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        sendByte(data[i]);
    }
}

void SerialPort::startReceiving()
{
    m_stopReceiving = false;
    m_receiveThread = std::thread(&SerialPort::receiveThreadProc, this);
}

void SerialPort::stopReceiving()
{
    if (m_receiveThread.joinable()) {
        m_stopReceiving = true;
        SetEvent(m_readEvent); // Wake up the thread
        m_receiveThread.join();
    }
}

void SerialPort::receiveThreadProc()
{
    uint8 buffer[256];
    DWORD bytesRead;
    
    while (!m_stopReceiving && isOpen()) {
        // Reset the event
        ResetEvent(m_readEvent);
        
        // Start an overlapped read
        BOOL result = ReadFile(m_handle, buffer, sizeof(buffer), &bytesRead, &m_readOverlapped);
        
        if (result) {
            // Read completed immediately
            for (DWORD i = 0; i < bytesRead; i++) {
                processReceivedByte(buffer[i]);
            }
        } else {
            DWORD error = GetLastError();
            if (error == ERROR_IO_PENDING) {
                // Wait for the read to complete or timeout
                DWORD waitResult = WaitForSingleObject(m_readEvent, 100);
                if (waitResult == WAIT_OBJECT_0) {
                    if (GetOverlappedResult(m_handle, &m_readOverlapped, &bytesRead, FALSE)) {
                        for (DWORD i = 0; i < bytesRead; i++) {
                            processReceivedByte(buffer[i]);
                        }
                    }
                }
            } else {
                dbglog("SerialPort::receiveThreadProc() - ReadFile failed, error %d\n", error);
                break;
            }
        }
    }
}

void SerialPort::processReceivedByte(uint8 byte)
{
    if (m_terminal) {
        // Feed the byte directly to the terminal's character processor
        // This replaces what IoCardTermMux::mxdToTermCallback() did
        m_terminal->processChar(byte);
    }
}

void SerialPort::transmitByte(uint8 byte)
{
    if (!isOpen()) {
        return;
    }
    
    m_txBusy = true;
    
    // Write the byte with overlapped I/O
    DWORD bytesWritten;
    BOOL result = WriteFile(m_handle, &byte, 1, &bytesWritten, &m_writeOverlapped);
    
    if (!result) {
        DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING) {
            // Wait for write to complete
            GetOverlappedResult(m_handle, &m_writeOverlapped, &bytesWritten, TRUE);
        } else {
            dbglog("SerialPort::transmitByte() - WriteFile failed, error %d\n", error);
            m_txBusy = false;
            return;
        }
    }
    
    // Schedule completion callback to model UART timing
    int64 delay = calculateTransmitDelay();
    m_txTimer = m_scheduler->createTimer(delay, 
                    std::bind(&SerialPort::onTransmitComplete, this));
}

void SerialPort::onTransmitComplete()
{
    m_txTimer = nullptr;
    
    std::lock_guard<std::mutex> lock(m_txMutex);
    m_txBusy = false;
    
    // Send next byte if queue has more data
    if (!m_txQueue.empty()) {
        uint8 nextByte = m_txQueue.front();
        m_txQueue.pop();
        transmitByte(nextByte);
    }
}

int64 SerialPort::calculateTransmitDelay() const
{
    // Calculate delay based on Wang 2200 serial timing
    // Formula: (start bit + data bits + parity bit + stop bits) / baud rate
    double bitsPerChar = 1.0; // start bit
    bitsPerChar += m_config.dataBits; // data bits
    if (m_config.parity != NOPARITY) {
        bitsPerChar += 1.0; // parity bit
    }
    bitsPerChar += (m_config.stopBits == ONESTOPBIT) ? 1.0 : 2.0; // stop bits
    
    // Convert to nanoseconds
    double charTimeUs = (bitsPerChar * 1.0E6) / m_config.baudRate;
    return static_cast<int64>(charTimeUs * 1000.0); // convert to nanoseconds
}

#else
// Non-Windows stub implementation
SerialPort::SerialPort(std::shared_ptr<Scheduler> scheduler) :
    m_scheduler(scheduler)
{
}

SerialPort::~SerialPort()
{
}

bool SerialPort::open(const SerialConfig &config)
{
    return false;
}

void SerialPort::close()
{
}

void SerialPort::attachTerminal(std::shared_ptr<Terminal> terminal)
{
}

void SerialPort::detachTerminal()
{
}

void SerialPort::sendByte(uint8 byte)
{
}

void SerialPort::sendData(const uint8 *data, size_t length)
{
}

void SerialPort::startReceiving()
{
}

void SerialPort::stopReceiving()
{
}

void SerialPort::receiveThreadProc()
{
}

void SerialPort::processReceivedByte(uint8 byte)
{
}

void SerialPort::transmitByte(uint8 byte)
{
}

void SerialPort::onTransmitComplete()
{
}

int64 SerialPort::calculateTransmitDelay() const
{
    return 0;
}
#endif // _WIN32