// Windows Serial Port implementation for Wang 2236 terminal communication
// Uses overlapped I/O for non-blocking serial communication

#include "SerialPort.h"
#include "../../shared/terminal/Terminal.h"
#include "../../core/system/Scheduler.h"
#include "host.h"  // for dbglog()

#ifdef _WIN32
#include <windows.h>
#include <cassert>
#include <string>
#include <thread>
#include <mutex>
#include <queue>

// Map platform-agnostic constants to Windows constants
#undef NOPARITY
#undef ODDPARITY  
#undef EVENPARITY
#undef ONESTOPBIT
#undef TWOSTOPBITS

const BYTE WIN_NOPARITY = ::NOPARITY;
const BYTE WIN_ODDPARITY = ::ODDPARITY;
const BYTE WIN_EVENPARITY = ::EVENPARITY;
const BYTE WIN_ONESTOPBIT = ::ONESTOPBIT;
const BYTE WIN_TWOSTOPBITS = ::TWOSTOPBITS;

static std::wstring toWinComPath(const std::string& name) {
    std::wstring w(name.begin(), name.end());
    if (w.rfind(L"COM", 0) == 0 && w.size() > 4) {
        return L"\\\\.\\" + w; // COM10 → \\.\COM10
    }
    return w;
}

SerialPort::SerialPort(std::shared_ptr<Scheduler> scheduler) :
    m_scheduler(std::move(scheduler)),
    m_handle(INVALID_HANDLE_VALUE),
    m_stopReceiving(false),
    m_txBusy(false),
    m_txTimer(nullptr)
{
    // init OVERLAPPED + manual-reset events
    ZeroMemory(&m_readOverlapped,  sizeof(m_readOverlapped));
    ZeroMemory(&m_writeOverlapped, sizeof(m_writeOverlapped));
    m_readEvent  = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    m_writeEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    m_readOverlapped.hEvent  = m_readEvent;
    m_writeOverlapped.hEvent = m_writeEvent;
}

SerialPort::~SerialPort()
{
    close();
    if (m_readEvent)  { CloseHandle(m_readEvent);  m_readEvent  = nullptr; }
    if (m_writeEvent) { CloseHandle(m_writeEvent); m_writeEvent = nullptr; }
}

bool SerialPort::open(const SerialConfig &config)
{
    if (isOpen()) {
        close();
    }

    m_config = config;

    std::wstring path = toWinComPath(config.portName);
    m_handle = CreateFileW(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,                      // exclusive
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,   // we use overlapped I/O
        nullptr
    );
    if (m_handle == INVALID_HANDLE_VALUE) {
        dbglog("SerialPort::open() - Failed to open %s, error %lu\n",
               config.portName.c_str(), GetLastError());
        return false;
    }

    // Basic driver buffers
    SetupComm(m_handle, 1<<16, 1<<16);

    // Configure DCB
    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(m_handle, &dcb)) {
        dbglog("SerialPort::open() - GetCommState failed, err %lu\n", GetLastError());
        close();
        return false;
    }

    dcb.BaudRate = config.baudRate;
    dcb.ByteSize = static_cast<BYTE>(config.dataBits);
    
    // Map platform-agnostic parity to Windows constants
    switch (config.parity) {
        case NOPARITY:  dcb.Parity = WIN_NOPARITY; break;
        case ODDPARITY: dcb.Parity = WIN_ODDPARITY; break;
        case EVENPARITY: dcb.Parity = WIN_EVENPARITY; break;
    }
    dcb.fParity = (config.parity != NOPARITY);
    
    // Map platform-agnostic stop bits to Windows constants
    switch (config.stopBits) {
        case ONESTOPBIT:  dcb.StopBits = WIN_ONESTOPBIT; break;
        case TWOSTOPBITS: dcb.StopBits = WIN_TWOSTOPBITS; break;
    }

    // Default: assert RTS/DTR, no flow control
    dcb.fOutxCtsFlow   = FALSE;
    dcb.fRtsControl    = RTS_CONTROL_ENABLE;   // keep RTS asserted
    dcb.fDtrControl    = DTR_CONTROL_ENABLE;   // keep DTR asserted
    dcb.fOutX = dcb.fInX = FALSE;
    dcb.fDsrSensitivity = FALSE;

    // Optional hardware CTS flow control (not used for Wang terminals)
    if (config.hwFlowControl) {
        dcb.fOutxCtsFlow = TRUE;
        dcb.fRtsControl  = RTS_CONTROL_HANDSHAKE;
    }

    // Optional software XON/XOFF flow control (recommended for Wang terminals)
    if (config.swFlowControl) {
        dcb.fOutX = TRUE;   // Pause transmission when XOFF received
        dcb.fInX  = TRUE;   // Send XON/XOFF when RX buffer thresholds reached
        dcb.XonChar  = 0x11; // XON character (DC1)
        dcb.XoffChar = 0x13; // XOFF character (DC3)
        dcb.XonLim   = 512;  // Send XON when RX buffer has this much free space
        dcb.XoffLim  = 128;  // Send XOFF when RX buffer has this much data
    }

    if (!SetCommState(m_handle, &dcb)) {
        dbglog("SerialPort::open() - SetCommState failed, err %lu\n", GetLastError());
        close();
        return false;
    }

    // Also assert RTS/DTR explicitly (some drivers care)
    EscapeCommFunction(m_handle, SETRTS);
    EscapeCommFunction(m_handle, SETDTR);

    // Reasonable timeouts (overlapped ignores most of these, but harmless)
    COMMTIMEOUTS to{};
    to.ReadIntervalTimeout         = MAXDWORD; // non-aggregating read
    to.ReadTotalTimeoutMultiplier  = 0;
    to.ReadTotalTimeoutConstant    = 0;
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant   = 0;
    if (!SetCommTimeouts(m_handle, &to)) {
        dbglog("SerialPort::open() - SetCommTimeouts failed, err %lu\n", GetLastError());
        close();
        return false;
    }

    PurgeComm(m_handle, PURGE_RXCLEAR | PURGE_TXCLEAR);

    // Start RX thread
    startReceiving();

    dbglog("SerialPort::open() - Opened %s at %d baud, %d%c%d, flow %s\n",
           config.portName.c_str(), config.baudRate, config.dataBits,
           (config.parity==ODDPARITY ? 'O' : (config.parity==EVENPARITY ? 'E' : 'N')),
           (config.stopBits==ONESTOPBIT ? 1 : 2),
           config.hwFlowControl && config.swFlowControl ? "RTS/CTS+XON/XOFF" :
           config.hwFlowControl ? "RTS/CTS" :
           config.swFlowControl ? "XON/XOFF" : "none");
    return true;
}

void SerialPort::close()
{
    if (!isOpen()) return;

    // Stop RX thread first to avoid race with CancelIo
    stopReceiving();

    // Cancel any outstanding I/O on this handle
    CancelIo(m_handle);

    CloseHandle(m_handle);
    m_handle = INVALID_HANDLE_VALUE;

    // Clear TX state/queue
    {
        std::lock_guard<std::recursive_mutex> lock(m_txMutex);
        std::queue<uint8> empty;
        std::swap(m_txQueue, empty);
        m_txBusy  = false;
        m_txTimer = nullptr;
    }

    dbglog("SerialPort::close() - Closed %s\n", m_config.portName.c_str());
}

void SerialPort::attachTerminal(std::shared_ptr<Terminal> terminal)
{
    m_terminal = std::move(terminal);
}

void SerialPort::detachTerminal()
{
    m_terminal.reset();
}

void SerialPort::setReceiveCallback(RxCallback cb)
{
    m_rxCallback = std::move(cb);
}

void SerialPort::sendByte(uint8 byte)
{
    if (!isOpen()) {
        dbglog("SerialPort::sendByte() - port closed, drop 0x%02X\n", byte);
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(m_txMutex);

    // Use configurable TX queue size
    if (m_txQueue.size() >= m_config.txQueueSize) {
        dbglog("SerialPort::sendByte() - TX queue full (%u), drop 0x%02X\n",
               (unsigned)m_txQueue.size(), byte);
        return;
    }

    m_txQueue.push(byte);

    if (!m_txBusy) {
        // kick off first byte
        uint8 b = m_txQueue.front();
        m_txQueue.pop();
        transmitByte(b);
    }
}

void SerialPort::sendData(const uint8 *data, size_t length)
{
    for (size_t i=0; i<length; ++i) sendByte(data[i]);
}

void SerialPort::sendXON()
{
    if (m_xoffSent.load()) {
        sendByte(0x11); // DC1 (XON)
        m_xoffSent.store(false);
        m_xonSentCount.fetch_add(1);
        
        // Capture for debugging if enabled
        if (m_captureCallback) {
            // Log this as a special flow control event
            dbglog("SerialPort::sendXON() - Sending XON to %s\n", m_config.portName.c_str());
        }
    }
}

void SerialPort::sendXOFF()
{
    if (!m_xoffSent.load()) {
        sendByte(0x13); // DC3 (XOFF)
        m_xoffSent.store(true);
        m_xoffSentCount.fetch_add(1);
        
        // Capture for debugging if enabled
        if (m_captureCallback) {
            // Log this as a special flow control event
            dbglog("SerialPort::sendXOFF() - Sending XOFF to %s\n", m_config.portName.c_str());
        }
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
        SetEvent(m_readEvent); // wake wait
        m_receiveThread.join();
    }
}

void SerialPort::receiveThreadProc()
{
    uint8  buffer[512];
    DWORD  bytesRead = 0;

    while (!m_stopReceiving && isOpen()) {
        // reset event and issue overlapped read
        ResetEvent(m_readEvent);
        BOOL ok = ReadFile(m_handle, buffer, sizeof(buffer), &bytesRead, &m_readOverlapped);
        if (ok) {
            // immediate completion
            for (DWORD i=0; i<bytesRead; ++i) processReceivedByte(buffer[i]);
            continue;
        }

        DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING) {
            DWORD wait = WaitForSingleObject(m_readEvent, 100); // poll ~10Hz - good balance for terminal communication
            if (wait == WAIT_OBJECT_0) {
                if (GetOverlappedResult(m_handle, &m_readOverlapped, &bytesRead, FALSE)) {
                    for (DWORD i=0; i<bytesRead; ++i) processReceivedByte(buffer[i]);
                }
            }
            continue;
        }

        dbglog("SerialPort::receiveThreadProc - ReadFile failed, err %lu\n", err);
        break; // bail on hard error
    }
}

void SerialPort::processReceivedByte(uint8 byte)
{
    // Increment RX counter
    m_rxByteCount.fetch_add(1);

    // Track activity for adaptive timing
    {
        std::lock_guard<std::mutex> lock(m_activityMutex);
        m_lastRxTime = std::chrono::steady_clock::now();
    }
    m_recentRxBytes.fetch_add(1);

    // Capture for debugging if enabled
    if (m_captureCallback) {
        m_captureCallback(byte, true);  // true = RX
    }

    // Send to MXD callback first (for COM port mode)
    if (m_rxCallback) {
        m_rxCallback(byte);
    }
    
    // Also send to terminal if one is attached (legacy mode)
#ifndef HEADLESS_BUILD
    if (m_terminal) {
        m_terminal->processChar(byte);
    }
#endif
}

void SerialPort::transmitByte(uint8 byte)
{
    if (!isOpen()) return;
    
    // Capture for debugging if enabled
    if (m_captureCallback) {
        m_captureCallback(byte, false);  // false = TX
    }

    // single in-flight write only; prepare OVERLAPPED
    ResetEvent(m_writeEvent);

    {
        std::lock_guard<std::recursive_mutex> lock(m_txMutex);
        m_txBusy = true;
    }

    DWORD written = 0;
    BOOL ok = WriteFile(m_handle, &byte, 1, &written, &m_writeOverlapped);
    if (!ok) {
        DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING) {
            dbglog("SerialPort::transmitByte - WriteFile failed, err %lu\n", err);
            std::lock_guard<std::recursive_mutex> lock(m_txMutex);
            m_txBusy = false;
            return;
        }
        // pending: completion will be detected/polled in onTransmitComplete()
    }

    // Model UART character time; when the timer fires we check completion
    int64 delay = calculateTransmitDelay();
    if (delay < 1000000) delay = 1000000; // floor ~1ms so we don't spin
    m_txTimer = m_scheduler->createTimer(
        delay,
        std::bind(&SerialPort::onTransmitComplete, this)
    );
}

void SerialPort::onTransmitComplete()
{
    // If the port is closed (or closing), just drain & bail.
    if (!isOpen()) {
        std::lock_guard<std::recursive_mutex> lock(m_txMutex);
        m_txBusy = false;
        std::queue<uint8> empty;
        std::swap(m_txQueue, empty);
        m_txTimer = nullptr;
        return;
    }

    DWORD bytes = 0;
    BOOL done = GetOverlappedResult(m_handle, &m_writeOverlapped, &bytes, FALSE);
    if (!done) {
        DWORD err = GetLastError();
        if (err == ERROR_IO_INCOMPLETE) {
            // Still pending (e.g., CTS low). Poll again shortly.
            m_txTimer = m_scheduler->createTimer(
                1000000, // 1 ms
                std::bind(&SerialPort::onTransmitComplete, this)
            );
            return;
        }
        // If we got here, the write was aborted or the handle changed.
        // Treat as hard-stop for this byte and continue with the queue.
        // (Common errors here: ERROR_OPERATION_ABORTED, ERROR_INVALID_HANDLE)
        dbglog("SerialPort::onTransmitComplete - write aborted/err %lu\n", err);
    } else {
        // Successfully transmitted a byte
        m_txByteCount.fetch_add(1);
    }

    uint8 next = 0;
    bool  have = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_txMutex);
        m_txBusy = false;
        if (!m_txQueue.empty()) {
            next = m_txQueue.front();
            m_txQueue.pop();
            have = true;
            m_txBusy = true; // claim for next write
        }
    }
    if (have) {
        transmitByte(next);
    }
}

int64 SerialPort::calculateTransmitDelay() const
{
    // (start + data + parity + stop) / baud → seconds
    double bitsPerChar = 1.0;                  // start
    bitsPerChar += m_config.dataBits;          // data
    if (m_config.parity != NOPARITY) bitsPerChar += 1.0;
    bitsPerChar += (m_config.stopBits == ONESTOPBIT) ? 1.0 : 2.0;

    // return nanoseconds (scheduler units)
    double charTimeUs = (bitsPerChar * 1.0e6) / m_config.baudRate;
    return static_cast<int64>(charTimeUs * 1000.0);
}

bool SerialPort::isOpen() const 
{
    return m_handle != INVALID_HANDLE_VALUE;
}

size_t SerialPort::getTxQueueSize() const
{
    std::lock_guard<std::recursive_mutex> lock(m_txMutex);
    return m_txQueue.size();
}

bool SerialPort::isTxQueueNearFull(float threshold) const
{
    size_t current_size = getTxQueueSize();
    return current_size >= (m_config.txQueueSize * threshold);
}

void SerialPort::flushTxQueue()
{
    std::lock_guard<std::recursive_mutex> lock(m_txMutex);
    // Clear the TX queue without sending the bytes
    while (!m_txQueue.empty()) {
        m_txQueue.pop();
    }
    dbglog("SerialPort::flushTxQueue() - Cleared TX queue for %s\n", m_config.portName.c_str());
}

#else
// POSIX Serial Port implementation for Linux/Unix systems
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/select.h>
#include <poll.h>
#include <errno.h>
#include <cstring>
#include <cassert>
// POSIX implementation helper functions
static speed_t baudRateToSpeed(uint32_t baudRate) {
    switch (baudRate) {
        case 300:    return B300;
        case 1200:   return B1200;
        case 2400:   return B2400;
        case 4800:   return B4800;
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        default:
            dbglog("SerialPort: Unsupported baud rate %u, using 19200\n", baudRate);
            return B19200;
    }
}

SerialPort::SerialPort(std::shared_ptr<Scheduler> scheduler) :
    m_scheduler(std::move(scheduler)),
    m_fd(-1),
    m_stopReceiving(false)
{
    // Initialize cancellation pipe
    m_cancelPipe[0] = m_cancelPipe[1] = -1;
    if (pipe(m_cancelPipe) == -1) {
        dbglog("SerialPort: Failed to create cancellation pipe: %s\n", strerror(errno));
    }
}

SerialPort::~SerialPort()
{
    close();
    if (m_cancelPipe[0] != -1) {
        ::close(m_cancelPipe[0]);
        m_cancelPipe[0] = -1;
    }
    if (m_cancelPipe[1] != -1) {
        ::close(m_cancelPipe[1]);
        m_cancelPipe[1] = -1;
    }
}

bool SerialPort::open(const SerialConfig &config)
{
    if (isOpen()) {
        close();
    }

    m_config = config;

    // Open the serial port in blocking mode for more efficient I/O
    m_fd = ::open(config.portName.c_str(), O_RDWR | O_NOCTTY);
    if (m_fd == -1) {
        dbglog("SerialPort::open() - Failed to open %s: %s\n",
               config.portName.c_str(), strerror(errno));
        return false;
    }

    // Configure the port
    struct termios tty;
    if (tcgetattr(m_fd, &tty) != 0) {
        dbglog("SerialPort::open() - tcgetattr failed: %s\n", strerror(errno));
        close();
        return false;
    }

    // Clear all flags and set basic configuration
    tty.c_cflag &= ~(CSIZE | PARENB | PARODD | CSTOPB | CRTSCTS);
    tty.c_iflag &= ~(IXON | IXOFF | IXANY | IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
    tty.c_oflag &= ~OPOST;
    tty.c_lflag &= ~(ICANON | ECHO | ECHONL | ISIG | IEXTEN);

    // Set data bits
    switch (config.dataBits) {
        case 7: tty.c_cflag |= CS7; break;
        case 8: tty.c_cflag |= CS8; break;
        default:
            dbglog("SerialPort::open() - Invalid data bits %d, using 8\n", config.dataBits);
            tty.c_cflag |= CS8;
            break;
    }

    // Set parity
    switch (config.parity) {
        case NOPARITY:
            // already cleared PARENB above
            break;
        case ODDPARITY:
            tty.c_cflag |= (PARENB | PARODD);
            break;
        case EVENPARITY:
            tty.c_cflag |= PARENB;
            tty.c_cflag &= ~PARODD;
            break;
    }

    // Set stop bits
    switch (config.stopBits) {
        case ONESTOPBIT:
            // already cleared CSTOPB above
            break;
        case TWOSTOPBITS:
            tty.c_cflag |= CSTOPB;
            break;
    }

    // Enable receiver and set local mode
    tty.c_cflag |= (CREAD | CLOCAL);

    // Hardware flow control (disabled by default for Wang terminals)
    if (config.hwFlowControl) {
        tty.c_cflag |= CRTSCTS;
    }

    // Software flow control (XON/XOFF)
    if (config.swFlowControl) {
        tty.c_iflag |= (IXON | IXOFF);
    }

    // Set baud rate
    speed_t speed = baudRateToSpeed(config.baudRate);
    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);

    // Set blocking mode for byte-granular reads with minimal timeout
    tty.c_cc[VMIN] = 1;   // Block until at least 1 byte available
    tty.c_cc[VTIME] = 1;  // 0.1 second inter-byte timeout (keeps latency low)

    // Apply the configuration
    if (tcsetattr(m_fd, TCSANOW, &tty) != 0) {
        dbglog("SerialPort::open() - tcsetattr failed: %s\n", strerror(errno));
        close();
        return false;
    }

    // Flush any existing data
    tcflush(m_fd, TCIOFLUSH);

    // Start receiving thread
    startReceiving();
    
    // Reset reconnection state on successful connection
    m_connected.store(true);
    m_reconnectAttempts.store(0);

    dbglog("SerialPort::open() - Opened %s at %d baud, %d%c%d, flow %s\n",
           config.portName.c_str(), config.baudRate, config.dataBits,
           (config.parity==ODDPARITY ? 'O' : (config.parity==EVENPARITY ? 'E' : 'N')),
           (config.stopBits==ONESTOPBIT ? 1 : 2),
           config.hwFlowControl && config.swFlowControl ? "RTS/CTS+XON/XOFF" :
           config.hwFlowControl ? "RTS/CTS" :
           config.swFlowControl ? "XON/XOFF" : "none");

    return true;
}

void SerialPort::close()
{
    if (!isOpen()) return;

    // Stop receiving thread first
    stopReceiving();

    ::close(m_fd);
    m_fd = -1;

    // Clear TX buffer
    {
        std::lock_guard<std::recursive_mutex> lock(m_txMutex);
        m_outbuf.clear();
    }
    
    // Update connection state
    m_connected.store(false);

    dbglog("SerialPort::close() - Closed %s\n", m_config.portName.c_str());
}

bool SerialPort::isOpen() const
{
    return m_fd != -1;
}

size_t SerialPort::getTxQueueSize() const
{
    std::lock_guard<std::recursive_mutex> lock(m_txMutex);
    return m_outbuf.size();
}

bool SerialPort::isTxQueueNearFull(float threshold) const
{
    size_t current_size = getTxQueueSize();
    return current_size >= (m_config.txQueueSize * threshold);
}

void SerialPort::flushTxQueue()
{
    std::lock_guard<std::recursive_mutex> lock(m_txMutex);
    // Clear the TX buffer without sending the bytes
    m_outbuf.clear();
    dbglog("SerialPort::flushTxQueue() - Cleared TX buffer for %s\n", m_config.portName.c_str());
}

void SerialPort::attachTerminal(std::shared_ptr<Terminal> terminal)
{
    m_terminal = std::move(terminal);
}

void SerialPort::detachTerminal()
{
    m_terminal.reset();
}

void SerialPort::setReceiveCallback(RxCallback cb)
{
    m_rxCallback = std::move(cb);
}

void SerialPort::sendByte(uint8 byte)
{
    if (!isOpen()) {
        dbglog("SerialPort::sendByte() - port closed, drop 0x%02X\n", byte);
        return;
    }

    // Direct write for responsive terminal display
    ssize_t written = write(m_fd, &byte, 1);
    if (written == 1) {
        m_txByteCount.fetch_add(1);
        if (m_captureCallback) {
            m_captureCallback(byte, false); // false = TX
        }

        // Track activity for adaptive timing
        {
            std::lock_guard<std::mutex> lock(m_activityMutex);
            m_lastTxTime = std::chrono::steady_clock::now();
        }
        m_recentTxBytes.fetch_add(1);
    } else if (written == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        // Port busy, try enqueueTx as fallback
        enqueueTx(&byte, 1);
    } else if (written != 1) {
        dbglog("SerialPort::sendByte() - write failed: %s\n",
               written == -1 ? strerror(errno) : "partial write");
    }
}

void SerialPort::sendData(const uint8 *data, size_t length)
{
    if (!isOpen()) {
        dbglog("SerialPort::sendData() - port closed, drop %zu bytes\n", length);
        return;
    }

    // Try direct write first for responsive display
    ssize_t written = write(m_fd, data, length);
    if (written > 0) {
        m_txByteCount.fetch_add(written);
        if (m_captureCallback) {
            for (ssize_t i = 0; i < written; i++) {
                m_captureCallback(data[i], false); // false = TX
            }
        }

        // Track activity for adaptive timing
        {
            std::lock_guard<std::mutex> lock(m_activityMutex);
            m_lastTxTime = std::chrono::steady_clock::now();
        }
        m_recentTxBytes.fetch_add(written);

        // If partial write, queue the remaining data
        if (static_cast<size_t>(written) < length) {
            enqueueTx(data + written, length - written);
        }
    } else if (written == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        // Port busy, queue all data
        enqueueTx(data, length);
    } else {
        dbglog("SerialPort::sendData() - write failed: %s\n",
               written == -1 ? strerror(errno) : "write returned 0");
    }
}

void SerialPort::sendXON()
{
    if (m_xoffSent.load()) {
        sendByte(0x11); // DC1 (XON)
        m_xoffSent.store(false);
        m_xonSentCount.fetch_add(1);
        
        // Capture for debugging if enabled
        if (m_captureCallback) {
            // Log this as a special flow control event
            dbglog("SerialPort::sendXON() - Sending XON to %s\n", m_config.portName.c_str());
        }
    }
}

void SerialPort::sendXOFF()
{
    if (!m_xoffSent.load()) {
        sendByte(0x13); // DC3 (XOFF)
        m_xoffSent.store(true);
        m_xoffSentCount.fetch_add(1);
        
        // Capture for debugging if enabled
        if (m_captureCallback) {
            // Log this as a special flow control event
            dbglog("SerialPort::sendXOFF() - Sending XOFF to %s\n", m_config.portName.c_str());
        }
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
        // Signal cancellation pipe to wake up the receive thread
        if (m_cancelPipe[1] != -1) {
            char dummy = 1;
            ssize_t result = write(m_cancelPipe[1], &dummy, 1);
            (void)result; // suppress unused variable warning
        }
        m_receiveThread.join();
    }
}

void SerialPort::receiveThreadProc()
{
    uint8 buffer[512];
    pollfd pfds[2];
    int nfds = 1;

    // Setup poll descriptors
    pfds[0].fd = m_fd;
    pfds[0].events = POLLIN; // Always monitor for RX data

    if (m_cancelPipe[0] != -1) {
        pfds[1].fd = m_cancelPipe[0];
        pfds[1].events = POLLIN;
        nfds = 2;
    }

    while (!m_stopReceiving && isOpen()) {
        // Check if we have data to send and update POLLOUT accordingly
        {
            std::lock_guard<std::recursive_mutex> lock(m_txMutex);
            if (!m_outbuf.empty()) {
                pfds[0].events = POLLIN | POLLOUT; // Monitor both RX and TX
            } else {
                pfds[0].events = POLLIN; // Only monitor RX
            }
        }

        // Use poll() with 10ms timeout for responsive terminal display
        int result = poll(pfds, nfds, 10);
        
        if (result > 0) {
            // Check for cancellation signal
            if (nfds > 1 && (pfds[1].revents & POLLIN)) {
                char dummy;
                ssize_t readResult = read(m_cancelPipe[0], &dummy, 1);
                (void)readResult; // suppress unused variable warning
                break; // Exit thread
            }

            // Check for data on serial port
            if (pfds[0].revents & POLLIN) {
                ssize_t bytesRead = read(m_fd, buffer, sizeof(buffer));
                if (bytesRead > 0) {
                    for (ssize_t i = 0; i < bytesRead; ++i) {
                        processReceivedByte(buffer[i]);
                    }
                } else if (bytesRead == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    dbglog("SerialPort::receiveThreadProc - read failed: %s, attempting reconnection\n", strerror(errno));
                    m_connected.store(false);
                    
                    // Check if we should attempt reconnection
                    if (m_reconnectAttempts.load() < MAX_RECONNECT_ATTEMPTS) {
                        int delay = getReconnectDelayMs();
                        dbglog("SerialPort::receiveThreadProc - Reconnecting in %d ms (attempt %d/%d)\n", 
                               delay, m_reconnectAttempts.load() + 1, MAX_RECONNECT_ATTEMPTS);
                        
                        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                        m_reconnectAttempts.fetch_add(1);
                        m_lastReconnectAttempt = std::chrono::steady_clock::now();
                        
                        if (attemptReconnect()) {
                            dbglog("SerialPort::receiveThreadProc - Reconnection successful\n");
                            continue; // Continue with the loop
                        } else {
                            dbglog("SerialPort::receiveThreadProc - Reconnection failed\n");
                        }
                    } else {
                        dbglog("SerialPort::receiveThreadProc - Max reconnection attempts exceeded\n");
                        break;
                    }
                } else if (bytesRead == 0) {
                    dbglog("SerialPort::receiveThreadProc - Port disconnected, attempting reconnection\n");
                    m_connected.store(false);
                    
                    // Check if we should attempt reconnection
                    if (m_reconnectAttempts.load() < MAX_RECONNECT_ATTEMPTS) {
                        int delay = getReconnectDelayMs();
                        dbglog("SerialPort::receiveThreadProc - Reconnecting in %d ms (attempt %d/%d)\n", 
                               delay, m_reconnectAttempts.load() + 1, MAX_RECONNECT_ATTEMPTS);
                        
                        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                        m_reconnectAttempts.fetch_add(1);
                        m_lastReconnectAttempt = std::chrono::steady_clock::now();
                        
                        if (attemptReconnect()) {
                            dbglog("SerialPort::receiveThreadProc - Reconnection successful\n");
                            continue; // Continue with the loop
                        } else {
                            dbglog("SerialPort::receiveThreadProc - Reconnection failed\n");
                        }
                    } else {
                        dbglog("SerialPort::receiveThreadProc - Max reconnection attempts exceeded\n");
                        break;
                    }
                }
            }

            // Check if we can send data (TX ready)
            if (pfds[0].revents & POLLOUT) {
                flushTxBuffer();
            }
        } else if (result == -1 && errno != EINTR) {
            dbglog("SerialPort::receiveThreadProc - select failed: %s, attempting reconnection\n", strerror(errno));
            m_connected.store(false);
            
            // Check if we should attempt reconnection
            if (m_reconnectAttempts.load() < MAX_RECONNECT_ATTEMPTS) {
                int delay = getReconnectDelayMs();
                dbglog("SerialPort::receiveThreadProc - Reconnecting in %d ms (attempt %d/%d)\n", 
                       delay, m_reconnectAttempts.load() + 1, MAX_RECONNECT_ATTEMPTS);
                
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                m_reconnectAttempts.fetch_add(1);
                m_lastReconnectAttempt = std::chrono::steady_clock::now();
                
                if (attemptReconnect()) {
                    dbglog("SerialPort::receiveThreadProc - Reconnection successful\n");
                    // Update poll descriptors after reconnection
                    pfds[0].fd = m_fd;
                    continue; // Continue with the loop
                } else {
                    dbglog("SerialPort::receiveThreadProc - Reconnection failed\n");
                }
            } else {
                dbglog("SerialPort::receiveThreadProc - Max reconnection attempts exceeded\n");
                break;
            }
        }
    }
}

void SerialPort::processReceivedByte(uint8 byte)
{
    // Increment RX counter
    m_rxByteCount.fetch_add(1);

    // Track activity for adaptive timing
    {
        std::lock_guard<std::mutex> lock(m_activityMutex);
        m_lastRxTime = std::chrono::steady_clock::now();
    }
    m_recentRxBytes.fetch_add(1);

    // Capture for debugging if enabled
    if (m_captureCallback) {
        m_captureCallback(byte, true);  // true = RX
    }

    // Send to MXD callback first (for COM port mode)
    if (m_rxCallback) {
        m_rxCallback(byte);
    }
    
    // Also send to terminal if one is attached (legacy mode)
#ifndef HEADLESS_BUILD
    if (m_terminal) {
        m_terminal->processChar(byte);
    }
#endif
}


bool SerialPort::attemptReconnect()
{
#ifdef _WIN32
    if (m_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
    }
    return open(m_config);
#else
    if (m_fd != -1) {
        ::close(m_fd);
        m_fd = -1;
    }
    return open(m_config);
#endif
}

void SerialPort::enqueueTx(const uint8_t* data, size_t len)
{
    if (!isOpen() || len == 0) {
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(m_txMutex);

    // Check buffer size limit
    if (m_outbuf.size() + len > m_config.txQueueSize) {
        dbglog("SerialPort::enqueueTx() - TX buffer full (%zu + %zu > %zu), dropping data\n",
               m_outbuf.size(), len, m_config.txQueueSize);
        return;
    }

    // Append data to output buffer
    m_outbuf.insert(m_outbuf.end(), data, data + len);

#ifdef WANGEMU_TX_DEBUG
    dbglog("SerialPort::enqueueTx() - Added %zu bytes, buffer now %zu bytes\n",
           len, m_outbuf.size());
#endif

    // Try immediate flush for responsive display (non-blocking)
    flushTxBuffer();
}

bool SerialPort::flushTxBuffer()
{
    if (!isOpen()) {
        return false;
    }

    std::lock_guard<std::recursive_mutex> lock(m_txMutex);

    if (m_outbuf.empty()) {
        return false; // Nothing to send
    }

    // Try to write as much as the kernel will accept
    ssize_t written = write(m_fd, m_outbuf.data(), m_outbuf.size());
    if (written > 0) {
        // Update TX byte counter
        m_txByteCount.fetch_add(written);

        // Remove sent bytes from buffer
        m_outbuf.erase(m_outbuf.begin(), m_outbuf.begin() + written);

#ifdef WANGEMU_TX_DEBUG
        dbglog("SerialPort::flushTxBuffer() - Wrote %zd bytes, %zu remain\n",
               written, m_outbuf.size());
#endif

        return true; // Successfully sent some data
    } else if (written == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        // Kernel buffer full, try again later
        return false;
    } else {
        // Real error
        dbglog("SerialPort::flushTxBuffer() - write failed: %s\n",
               written == -1 ? strerror(errno) : "partial write");
        return false;
    }
}

int SerialPort::getReconnectDelayMs() const
{
    int attempts = m_reconnectAttempts.load();
    // Exponential backoff: 250ms, 500ms, 1s, 2s, 4s, 8s, capped at 10s
    int delay = BASE_RECONNECT_DELAY_MS * (1 << std::min(attempts, 5));
    return std::min(delay, 10000); // Cap at 10 seconds
}

// Activity tracking for adaptive timing
bool SerialPort::hasRecentActivity()
{
    auto now = std::chrono::steady_clock::now();
    constexpr auto ACTIVITY_WINDOW = std::chrono::milliseconds(100);
    constexpr auto RESET_WINDOW = std::chrono::milliseconds(200);

    std::lock_guard<std::mutex> lock(m_activityMutex);

    // Periodically reset counters to prevent overflow and ensure fresh data
    static auto lastReset = std::chrono::steady_clock::time_point{};
    if (lastReset == std::chrono::steady_clock::time_point{} || now - lastReset > RESET_WINDOW) {
        m_recentTxBytes.store(0);
        m_recentRxBytes.store(0);
        lastReset = now;
    }

    // Simplified activity detection - any recent TX/RX within 100ms counts as activity
    // This prevents the freeze issues during gaming
    bool recentTx = (now - m_lastTxTime < ACTIVITY_WINDOW);
    bool recentRx = (now - m_lastRxTime < ACTIVITY_WINDOW);

    return recentTx || recentRx;  // Any recent activity counts
}

uint32_t SerialPort::getRecentTxBytes() const
{
    return m_recentTxBytes.load();
}

uint32_t SerialPort::getRecentRxBytes() const
{
    return m_recentRxBytes.load();
}

#endif // _WIN32
