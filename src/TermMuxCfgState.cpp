#include "TermMuxCfgState.h"
#include "Ui.h"                 // for UI_Alert
#include "host.h"

#include <sstream>
#include <string>
#include <cassert>

// ------------------------------------------------------------------------
// public members
// ------------------------------------------------------------------------

// assignment
TermMuxCfgState&
TermMuxCfgState::operator=(const TermMuxCfgState &rhs) noexcept
{
    // don't copy something that hasn't been initialized
    assert(rhs.m_initialized);

    // check for self-assignment
    if (this != &rhs) {
        m_num_terms   = rhs.m_num_terms;
        for (int i = 0; i < 4; i++) {
            m_terminals[i] = rhs.m_terminals[i];
        }
        m_initialized = true;
    }

    return *this;
}


// copy constructor
TermMuxCfgState::TermMuxCfgState(const TermMuxCfgState &obj) noexcept
{
    assert(obj.m_initialized);
    m_num_terms   = obj.m_num_terms;
    for (int i = 0; i < 4; i++) {
        m_terminals[i] = obj.m_terminals[i];
    }
    m_initialized = true;
}


// equality comparison
bool
TermMuxCfgState::operator==(const CardCfgState &rhs) const noexcept
{
    const TermMuxCfgState rrhs(dynamic_cast<const TermMuxCfgState&>(rhs));

    assert(     m_initialized);
    assert(rrhs.m_initialized);

    bool equal = (getNumTerminals() == rrhs.getNumTerminals());
    if (equal) {
        for (int i = 0; i < getNumTerminals(); i++) {
            if (m_terminals[i].com_port != rrhs.m_terminals[i].com_port ||
                m_terminals[i].baud_rate != rrhs.m_terminals[i].baud_rate ||
                m_terminals[i].flow_control != rrhs.m_terminals[i].flow_control ||
                m_terminals[i].sw_flow_control != rrhs.m_terminals[i].sw_flow_control) {
                equal = false;
                break;
            }
        }
    }
    return equal;
}


bool
TermMuxCfgState::operator!=(const CardCfgState &rhs) const noexcept
{
    return !(*this == rhs);
}


// establish a reasonable default state on a newly minted card
void
TermMuxCfgState::setDefaults() noexcept
{
    setNumTerminals(1);
}


// read from configuration file
void
TermMuxCfgState::loadIni(const std::string &subgroup)
{
    int ival;
    host::configReadInt(subgroup, "numTerminals", &ival, 1);
    if (ival < 1 || ival > 4) {
        UI_warn("config state messed up -- assuming something reasonable");
        ival = 1;
    }
    setNumTerminals(ival);
    
    // Load per-terminal COM port settings
    for (int i = 0; i < 4; i++) {
        std::ostringstream oss;
        oss << "terminal" << i << "_";
        std::string term_prefix = oss.str();
        
        std::string com_port;
        std::string default_port = "";  // Default empty port means GUI terminal
        host::configReadStr(subgroup, term_prefix + "com_port", &com_port, &default_port);
        m_terminals[i].com_port = com_port;
        
        int baud_rate;
        host::configReadInt(subgroup, term_prefix + "baud_rate", &baud_rate, 19200);
        m_terminals[i].baud_rate = baud_rate;
        
        int flow_control;
        host::configReadInt(subgroup, term_prefix + "flow_control", &flow_control, 0);
        m_terminals[i].flow_control = (flow_control != 0);
        
        int sw_flow_control;
        host::configReadInt(subgroup, term_prefix + "sw_flow_control", &sw_flow_control, 0);
        m_terminals[i].sw_flow_control = (sw_flow_control != 0);
    }
    
    m_initialized = true;
}


// save to configuration file
void
TermMuxCfgState::saveIni(const std::string &subgroup) const
{
    assert(m_initialized);
    host::configWriteInt(subgroup, "numTerminals", getNumTerminals());
    
    // Save per-terminal COM port settings
    for (int i = 0; i < 4; i++) {
        std::ostringstream oss;
        oss << "terminal" << i << "_";
        std::string term_prefix = oss.str();
        
        host::configWriteStr(subgroup, term_prefix + "com_port", m_terminals[i].com_port);
        host::configWriteInt(subgroup, term_prefix + "baud_rate", m_terminals[i].baud_rate);
        host::configWriteInt(subgroup, term_prefix + "flow_control", m_terminals[i].flow_control ? 1 : 0);
        host::configWriteInt(subgroup, term_prefix + "sw_flow_control", m_terminals[i].sw_flow_control ? 1 : 0);
    }
}


void
TermMuxCfgState::setNumTerminals(int count) noexcept
{
    assert(count >= 1 && count <= 4);
    m_num_terms   = count;
    m_initialized = true;
}


int
TermMuxCfgState::getNumTerminals() const noexcept
{
    return m_num_terms;
}


// return a copy of self
std::shared_ptr<CardCfgState>
TermMuxCfgState::clone() const
{
    return std::make_shared<TermMuxCfgState>(*this);
}


// returns true if the current configuration is reasonable, and false if not.
// if returning false, this routine first calls UI_Alert() describing what
// is wrong.
bool
TermMuxCfgState::configOk(bool /*warn*/) const noexcept
{
    return true;  // pretty hard to screw it up
}


// returns true if the state has changed in a way that requires a reboot
bool
TermMuxCfgState::needsReboot(const CardCfgState &other) const noexcept
{
    const TermMuxCfgState oother(dynamic_cast<const TermMuxCfgState&>(other));
    
    if (getNumTerminals() != oother.getNumTerminals()) {
        return true;
    }
    
    // Check if any terminal COM port configuration changed
    for (int i = 0; i < getNumTerminals(); i++) {
        if (m_terminals[i].com_port != oother.m_terminals[i].com_port ||
            m_terminals[i].baud_rate != oother.m_terminals[i].baud_rate ||
            m_terminals[i].flow_control != oother.m_terminals[i].flow_control ||
            m_terminals[i].sw_flow_control != oother.m_terminals[i].sw_flow_control) {
            return true;
        }
    }
    
    return false;
}

// ------------ Per-terminal COM port configuration methods ------------

void TermMuxCfgState::setTerminalComPort(int term_num, const std::string &port_name) noexcept
{
    assert(term_num >= 0 && term_num < 4);
    m_terminals[term_num].com_port = port_name;
}

std::string TermMuxCfgState::getTerminalComPort(int term_num) const noexcept
{
    assert(term_num >= 0 && term_num < 4);
    return m_terminals[term_num].com_port;
}

void TermMuxCfgState::setTerminalBaudRate(int term_num, int baud_rate) noexcept
{
    assert(term_num >= 0 && term_num < 4);
    m_terminals[term_num].baud_rate = baud_rate;
}

int TermMuxCfgState::getTerminalBaudRate(int term_num) const noexcept
{
    assert(term_num >= 0 && term_num < 4);
    return m_terminals[term_num].baud_rate;
}

void TermMuxCfgState::setTerminalFlowControl(int term_num, bool flow_control) noexcept
{
    assert(term_num >= 0 && term_num < 4);
    m_terminals[term_num].flow_control = flow_control;
}

bool TermMuxCfgState::getTerminalFlowControl(int term_num) const noexcept
{
    assert(term_num >= 0 && term_num < 4);
    return m_terminals[term_num].flow_control;
}

void TermMuxCfgState::setTerminalSwFlowControl(int term_num, bool sw_flow_control) noexcept
{
    assert(term_num >= 0 && term_num < 4);
    m_terminals[term_num].sw_flow_control = sw_flow_control;
}

bool TermMuxCfgState::getTerminalSwFlowControl(int term_num) const noexcept
{
    assert(term_num >= 0 && term_num < 4);
    return m_terminals[term_num].sw_flow_control;
}

bool TermMuxCfgState::isTerminalComPort(int term_num) const noexcept
{
    assert(term_num >= 0 && term_num < 4);
    return !m_terminals[term_num].com_port.empty();
}

// vim: ts=8:et:sw=4:smarttab
