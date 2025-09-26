#ifndef _INCLUDE_ITERM_SESSION_H_
#define _INCLUDE_ITERM_SESSION_H_

#include "../../core/system/w2200.h"
#include <functional>

/**
 * ITermSession - Terminal Session Interface
 * 
 * This interface provides an abstraction layer between the MXD (terminal multiplexer)
 * card and concrete terminal I/O implementations. It allows the MXD to send bytes
 * to different types of terminals (GUI terminal, serial terminal, etc.) without
 * knowing the specific implementation details.
 * 
 * The reverse path (Terminal → MXD) is handled via a callback function passed
 * to the terminal session implementation during construction.
 */
struct ITermSession {
    virtual ~ITermSession() = default;
    
    /**
     * Send a byte from MXD to the terminal
     * @param byte The byte to send to the terminal
     */
    virtual void mxdToTerm(uint8 byte) = 0;
    
    /**
     * Check if the session is currently active/connected
     * @return true if the session can send/receive data
     */
    virtual bool isActive() const = 0;
    
    /**
     * Get a human-readable description of this session
     * @return A string describing the session (e.g., "Serial:/dev/ttyUSB0", "GUI:Terminal1")
     */
    virtual std::string getDescription() const = 0;
};

/**
 * Callback type for terminal → MXD data flow
 * Called when the terminal sends a byte back to the MXD
 */
using TermToMxdCallback = std::function<void(uint8 byte)>;

#endif // _INCLUDE_ITERM_SESSION_H_