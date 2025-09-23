// This class is derived from CardCfgState and holds some configuration state
// particular to this type of card.  For the terminal mux, currently there is
// only one bit of state: how many terminals are connected to it.
//
// TODO: Other possible configuration options (per terminal):
//    - link speed  
//    - attached printer or not
// IMPLEMENTED:
//    - serial port instead of virtual terminal

#ifndef _INCLUDE_TERM_MUX_CFG_H_
#define _INCLUDE_TERM_MUX_CFG_H_

#include "CardCfgState.h"
#include <string>

class TermMuxCfgState : public CardCfgState
{
public:
    TermMuxCfgState() = default;
    TermMuxCfgState(const TermMuxCfgState &obj) noexcept;             // copy
    TermMuxCfgState &operator=(const TermMuxCfgState &rhs) noexcept;  // assign

    // ------------ common CardCfgState interface ------------
    // See CardCfgState.h for the use of these functions.

    void setDefaults() noexcept override;
    void loadIni(const std::string &subgroup) override;
    void saveIni(const std::string &subgroup) const override;
    bool operator==(const CardCfgState &rhs) const noexcept override;
    bool operator!=(const CardCfgState &rhs) const noexcept override;
    std::shared_ptr<CardCfgState> clone() const override;
    bool configOk(bool warn) const noexcept override;
    bool needsReboot(const CardCfgState &other) const noexcept override;

    // ------------ unique to TermMuxCfgState ------------

    // set/get the number of terminals associated with the mux
    void setNumTerminals(int count) noexcept;
    int  getNumTerminals() const noexcept;
    
    // per-terminal COM port configuration
    void setTerminalComPort(int term_num, const std::string &port_name) noexcept;
    std::string getTerminalComPort(int term_num) const noexcept;
    
    void setTerminalBaudRate(int term_num, int baud_rate) noexcept;
    int getTerminalBaudRate(int term_num) const noexcept;
    
    void setTerminalFlowControl(int term_num, bool flow_control) noexcept;
    bool getTerminalFlowControl(int term_num) const noexcept;
    
    void setTerminalSwFlowControl(int term_num, bool sw_flow_control) noexcept;
    bool getTerminalSwFlowControl(int term_num) const noexcept;
    
    // check if a terminal should use COM port instead of GUI window
    bool isTerminalComPort(int term_num) const noexcept;

private:
    struct TerminalCfg {
        std::string com_port = "";      // empty = use GUI window, non-empty = COM port name
        int baud_rate = 19200;
        bool flow_control = false;      // Hardware flow control (RTS/CTS) - not used for Wang terminals
        bool sw_flow_control = false;   // Software flow control (XON/XOFF) - recommended for Wang terminals
    };
    
    bool m_initialized = false;         // for debugging and sanity checking
    int  m_num_terms   = 0;             // number of terminals connected to term mux
    TerminalCfg m_terminals[4];         // per-terminal configuration (max 4 terminals)
};

#endif // _INCLUDE_TERM_MUX_CFG_H_

// vim: ts=8:et:sw=4:smarttab
