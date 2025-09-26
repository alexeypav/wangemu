// SerialTermSession - Serial Terminal Session Implementation
// 
// This implements the ITermSession interface for physical Wang terminals
// connected via USB-serial adapters, providing the abstraction layer
// between the MXD card and the actual serial port hardware.

#include "SerialTermSession.h"
#include "../../platform/common/host.h"  // for dbglog()
#include <sstream>

SerialTermSession::SerialTermSession(std::shared_ptr<SerialPort> serialPort,
                                     TermToMxdCallback onFromTerm) :
    m_serialPort(std::move(serialPort)),
    m_onFromTerm(std::move(onFromTerm)),
    m_rxBytes(0),
    m_txBytes(0)
{
    if (!m_serialPort) {
        dbglog("SerialTermSession: ERROR - null serial port provided\n");
        return;
    }
    
    // Set up the receive callback to forward data from terminal to MXD
    m_serialPort->setReceiveCallback(
        std::bind(&SerialTermSession::onSerialRx, this, std::placeholders::_1)
    );
    
    dbglog("SerialTermSession: Created session for %s\n", getDescription().c_str());
}

SerialTermSession::~SerialTermSession()
{
    if (m_serialPort) {
        // Clear the receive callback to avoid dangling pointer
        m_serialPort->setReceiveCallback(nullptr);
        dbglog("SerialTermSession: Destroyed session for %s (RX: %llu, TX: %llu bytes)\n",
               getDescription().c_str(), 
               (unsigned long long)m_rxBytes, 
               (unsigned long long)m_txBytes);
    }
}

void SerialTermSession::mxdToTerm(uint8 byte)
{
    if (!m_serialPort || !m_serialPort->isOpen()) {
        // Silently drop data if port is closed - this is normal during
        // startup/shutdown or when terminals are disconnected
        return;
    }
    
    m_serialPort->sendByte(byte);
}

bool SerialTermSession::isActive() const
{
    return m_serialPort && m_serialPort->isOpen();
}

std::string SerialTermSession::getDescription() const
{
    if (!m_serialPort) {
        return "Serial:NULL";
    }
    
    // Note: We can't access the serial config directly without adding
    // a getter method to SerialPort. For now, return a generic description.
    // In a real implementation, we'd add SerialPort::getConfig() method.
    std::ostringstream oss;
    oss << "Serial:" << (isActive() ? "Active" : "Inactive");
    return oss.str();
}

void SerialTermSession::getStats(uint64_t* rxBytes, uint64_t* txBytes) const
{
    if (m_serialPort) {
        if (rxBytes) *rxBytes = m_serialPort->getRxByteCount();
        if (txBytes) *txBytes = m_serialPort->getTxByteCount();
    } else {
        if (rxBytes) *rxBytes = 0;
        if (txBytes) *txBytes = 0;
    }
}

void SerialTermSession::onSerialRx(uint8 byte)
{
    // Forward the byte from terminal to MXD via callback
    if (m_onFromTerm) {
        m_onFromTerm(byte);
    }
}