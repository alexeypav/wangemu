#ifndef _INCLUDE_SERIAL_TERM_SESSION_H_
#define _INCLUDE_SERIAL_TERM_SESSION_H_

#include "ITermSession.h"
#include "SerialPort.h"
#include <memory>
#include <string>

/**
 * SerialTermSession - Serial Terminal Session Implementation
 * 
 * This class implements the ITermSession interface for physical Wang terminals
 * connected via serial ports (USB-serial adapters). It wraps a SerialPort
 * instance and provides the session abstraction needed by the MXD card.
 * 
 * Data flow:
 * - MXD → Terminal: mxdToTerm() calls SerialPort::sendByte()
 * - Terminal → MXD: SerialPort RX callback calls the TermToMxdCallback
 */
class SerialTermSession : public ITermSession
{
public:
    /**
     * Construct a serial terminal session
     * @param serialPort The serial port instance to use for communication
     * @param onFromTerm Callback to invoke when data is received from the terminal
     */
    SerialTermSession(std::shared_ptr<SerialPort> serialPort,
                      TermToMxdCallback onFromTerm);
    
    virtual ~SerialTermSession();

    // ITermSession interface
    void mxdToTerm(uint8 byte) override;
    bool isActive() const override;
    std::string getDescription() const override;
    
    /**
     * Get the underlying serial port instance
     * @return Shared pointer to the serial port
     */
    std::shared_ptr<SerialPort> getSerialPort() const { return m_serialPort; }
    
    /**
     * Get statistics about this session
     * @param rxBytes Output parameter for received byte count
     * @param txBytes Output parameter for transmitted byte count
     */
    void getStats(uint64_t* rxBytes, uint64_t* txBytes) const;
    
private:
    std::shared_ptr<SerialPort> m_serialPort;
    TermToMxdCallback m_onFromTerm;
    
    // Statistics
    mutable uint64_t m_rxBytes;
    mutable uint64_t m_txBytes;
    
    // Internal callback for SerialPort RX
    void onSerialRx(uint8 byte);
};

#endif // _INCLUDE_SERIAL_TERM_SESSION_H_