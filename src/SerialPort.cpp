// Windows Serial Port implementation for Wang 2236 terminal communication
// Uses overlapped I/O for non-blocking serial communication

#include "SerialPort.h"
#include "Terminal.h"
#include "Scheduler.h"
#include "host.h"  // for dbglog()

#ifdef _WIN32
#include <windows.h>
#include <cassert>
#include <string>
#include <thread>
#include <mutex>
#include <queue>

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
    dcb.Parity   = static_cast<BYTE>(config.parity);
    dcb.fParity  = (config.parity != NOPARITY);
    dcb.StopBits = static_cast<BYTE>(config.stopBits);

    // Default: assert RTS/DTR, no flow control
    dcb.fOutxCtsFlow   = FALSE;
    dcb.fRtsControl    = RTS_CONTROL_ENABLE;   // keep RTS asserted
    dcb.fDtrControl    = DTR_CONTROL_ENABLE;   // keep DTR asserted
    dcb.fOutX = dcb.fInX = FALSE;
    dcb.fDsrSensitivity = FALSE;

    // Optional hardware CTS flow control
    if (config.hwFlowControl) {
        dcb.fOutxCtsFlow = TRUE;
        dcb.fRtsControl  = RTS_CONTROL_HANDSHAKE;
    }

    // Optional software XON/XOFF flow control
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

    // simple backpressure cap
    constexpr size_t MAX_TX_QUEUE = 512;
    if (m_txQueue.size() >= MAX_TX_QUEUE) {
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
            DWORD wait = WaitForSingleObject(m_readEvent, 100); // poll ~10Hz
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
    // Send to MXD callback first (for COM port mode)
    if (m_rxCallback) {
        m_rxCallback(byte);
    }
    
    // Also send to terminal if one is attached (legacy mode)
    if (m_terminal) {
        m_terminal->processChar(byte);
    }
}

void SerialPort::transmitByte(uint8 byte)
{
    if (!isOpen()) return;

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

#else
// Non-Windows stub implementation
SerialPort::SerialPort(std::shared_ptr<Scheduler> scheduler) : m_scheduler(std::move(scheduler)) {}
SerialPort::~SerialPort() {}
bool  SerialPort::open(const SerialConfig &) { return false; }
void  SerialPort::close() {}
void  SerialPort::attachTerminal(std::shared_ptr<Terminal>) {}
void  SerialPort::detachTerminal() {}
void  SerialPort::setReceiveCallback(RxCallback) {}
void  SerialPort::sendByte(uint8) {}
void  SerialPort::sendData(const uint8*, size_t) {}
void  SerialPort::startReceiving() {}
void  SerialPort::stopReceiving() {}
void  SerialPort::receiveThreadProc() {}
void  SerialPort::processReceivedByte(uint8) {}
void  SerialPort::transmitByte(uint8) {}
void  SerialPort::onTransmitComplete() {}
int64 SerialPort::calculateTransmitDelay() const { return 0; }
#endif // _WIN32
