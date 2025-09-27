// The MXD Terminal Mux card contains an 8080, some EPROM and some RAM,
// along with four RS-232 ports.  The function is emulated at the chip level,
// meaning and embedded i8080 microprocessor emulates the actual ROM image
// from a real MXD card.
//
// These documents were especially useful in reverse engineering the card:
// - https://wang2200.org/docs/system/2200MVP_MaintenanceManual.729-0584-A.1-84.pdf
//     section F, page 336..., has schematics for the MXD board (7290-1, 7291-1)
// - https://wang2200.org/docs/internal/2236MXE_Documentation.8-83.pdf
// - Hand disassembly of MXD ROM image:
//     https://wang2200.org/2200tech/wang_2236mxd.lst

#include <algorithm>  // for std::min

#ifdef _WIN32
#define NOMINMAX  // Prevent Windows from defining min/max macros
#endif

#include "../cpu/Cpu2200.h"
#include "IoCardKeyboard.h"   // for key encodings
#include "IoCardTermMux.h"
#include "../system/Scheduler.h"
#include "../../shared/config/TermMuxCfgState.h"
#ifndef HEADLESS_BUILD
#include "../../shared/terminal/Terminal.h"
#endif
#include "../../platform/common/SerialPort.h"       // for COM port terminals
#include "../../headless/session/ITermSession.h"     // for session abstraction
#include "../../gui/system/Ui.h"
#include "../../platform/common/host.h"             // for dbglog()
#include "../cpu/i8080.h"
#include "../system/system2200.h"

bool do_dbg = false;

#ifdef _MSC_VER
#pragma warning( disable: 4127 )  // conditional expression is constant
#endif

// the i8080 runs at 1.78 MHz
const int NS_PER_TICK = 561;

// Serial character transmission time (for terminals at 19200 baud)
// 11 bits per character (start + 8 data + odd parity + stop) at 19200 bps
static const int64 SERIAL_CHAR_DELAY = TIMER_US(11.0 * 1.0E6 / 19200.0);

// mxd eprom image
#include "IoCardTermMux_eprom.h"

// input port defines
static const int IN_UART_TXRDY  = 0x00; // parallel poll of which UARTs have room in their tx fifo
static const int IN_2200_STATUS = 0x01; // various status bits
        //   0x01 = OBS strobe seen
        //   0x02 = CBS strobe seen
        //   0x04 = PRIME (reset) strobe seen (cleared by OUT 0)
        //   0x08 = high means we are selected and the CPU is waiting for input
        //   0x10 = board selected at time of ABS
        //   0x20 = AB1 when ABS strobed
        //   0x40 = AB2 when ABS strobed
        //   0x80 = AB3 when ABS strobed
static const int IN_OBUS_N      = 0x02; // read !OB bus, and clear obs/cbs strobe status
static const int IN_OBSCBS_ADDR = 0x03; // [7:5] = [AB3:AB1] at time of cbs/obs strobe (active low?)
static const int IN_UART_RXRDY  = 0x04; // parallel poll of which UARTs have received a byte
static const int IN_UART_DATA   = 0x06;
static const int IN_UART_STATUS = 0x0E;
        //   0x80 = DSR       (data set ready)
        //   0x40 = BRKDET    (break detect)
        //   0x20 = FE        (framing error)
        //   0x10 = OE        (overrun error)
        //   0x08 = PE        (parity error)
        //   0x04 = TxEMPTY   (the tx fifo is empty and the serializer is done)
        //   0x02 = RxRDY     (a byte has been received)
        //   0x01 = TxRDY     (the tx fifo buffer has room for a character)

// output port defines
static const int OUT_CLR_PRIME = 0x00;  // clears reset latch
static const int OUT_IB_N      = 0x01;  // drive !IB1-!IB8, pulse IBS
static const int OUT_IB9_N     = 0x11;  // same as OUT_IB_N, plus drive IB9
static const int OUT_PRIME     = 0x02;  // fires the !PRIME strobe
static const int OUT_HALT_STEP = 0x03;  // one shot strobe
static const int OUT_UART_SEL  = 0x05;  // uart chip select
static const int OUT_UART_DATA = 0x06;  // write to selected uart data reg
static const int OUT_RBI       = 0x07;  // 0=ready/1=busy; bit n=addr (n+1); bit 7=n/c
                                        // 01=1(kb),02=2(status),04=3(n/c),08=4(prt),10=5(vpcrt),20=6(cmd),40=7(crt)
static const int OUT_UART_CMD  = 0x0E;  // write to selected uart command reg

// instance constructor
IoCardTermMux::IoCardTermMux(std::shared_ptr<Scheduler> scheduler,
                             std::shared_ptr<Cpu2200> cpu,
                             int base_addr, int card_slot,
                             const CardCfgState *cfg) :
    m_scheduler(scheduler),
    m_cpu(cpu),
    m_base_addr(base_addr),
    m_slot(card_slot)
{
    if (m_slot < 0) {
        // this is just a probe to query properties, so don't make a window
        return;
    }

    // TermMux configuration state
    assert(cfg != nullptr);
    const TermMuxCfgState *cp = dynamic_cast<const TermMuxCfgState*>(cfg);
    assert(cp != nullptr);
    m_cfg = *cp;
    // TODO: why have m_num_terms if we have access to m_cfg?
    m_num_terms = m_cfg.getNumTerminals();
    assert(1 <= m_num_terms && m_num_terms <= 4);

    for (auto &t : m_terms) {
#ifndef HEADLESS_BUILD
        t.terminal = nullptr;
#endif
        t.serial_port.reset();
        t.session.reset();

        t.rx_ready = false;
        t.rx_byte  = 0x00;

        // Initialize flow control state
        t.xoff_sent = false;
        t.xoff_sent_count = 0;
        t.xon_sent_count = 0;

        t.tx_ready = true;
        t.tx_byte  = 0x00;
        t.tx_tmr   = nullptr;
    }

    int io_addr = 0;
    const bool ok = system2200::getSlotInfo(card_slot, nullptr, &io_addr);
    assert(ok);

    m_i8080 = i8080_new(IoCardTermMux::i8080_rd_func,
                        IoCardTermMux::i8080_wr_func,
                        IoCardTermMux::i8080_in_func,
                        IoCardTermMux::i8080_out_func,
                        this);
    assert(m_i8080);
    i8080_reset(static_cast<i8080*>(m_i8080));

    // register the i8080 for clock callback
    clkCallback cb = std::bind(&IoCardTermMux::execOneOp, this);
    system2200::registerClockedDevice(cb);

    // create all the terminals
    auto const cpu_type = m_cpu->getCpuType();
    [[maybe_unused]] const bool vp_mode = (cpu_type != Cpu2200::CPUTYPE_2200B)
                      && (cpu_type != Cpu2200::CPUTYPE_2200T);
    for(int n=0; n<m_num_terms; n++) {
        // Check if this terminal should use COM port
        if (m_cfg.isTerminalComPort(n)) {
            // Create and configure the serial port
            auto serial_port = std::make_shared<SerialPort>(scheduler);
            
            SerialConfig serial_cfg;
            serial_cfg.portName = m_cfg.getTerminalComPort(n);
            serial_cfg.baudRate = m_cfg.getTerminalBaudRate(n);
            // Hardware flow control (RTS/CTS) is disabled for Wang terminals since they don't support it
            serial_cfg.hwFlowControl = false;
            serial_cfg.swFlowControl = m_cfg.getTerminalSwFlowControl(n);
            serial_cfg.dataBits = 8;
#ifdef _WIN32
            serial_cfg.parity = ODDPARITY;  // Wang terminals typically use odd parity
            serial_cfg.stopBits = ONESTOPBIT;
#endif
            
            if (serial_port->open(serial_cfg)) {
                // Store the serial port - NO GUI terminal for COM mode
                m_terms[n].serial_port = serial_port;
#ifndef HEADLESS_BUILD
                m_terms[n].terminal.reset();  // No GUI terminal
#endif
                
                // Set up RX callback so serial → MXD works (using new FIFO-based method)
                serial_port->setReceiveCallback(
                    [this, n](uint8 b){ this->serialRxByte(n, b); });
                
                dbglog("IoCardTermMux: Terminal %d connected to COM port %s at %d baud\n",
                       n, m_cfg.getTerminalComPort(n).c_str(), m_cfg.getTerminalBaudRate(n));
                continue;  // Skip GUI terminal creation
            } else {
                dbglog("IoCardTermMux: Failed to open COM port %s for terminal %d, terminal available for session connection\n",
                       m_cfg.getTerminalComPort(n).c_str(), n);
                // Fall through to GUI terminal creation
            }
        }
        
        // Create standard GUI terminal (fallback or when no COM port configured)
        m_terms[n].serial_port.reset();  // Ensure no serial port
#ifndef HEADLESS_BUILD
        m_terms[n].terminal = std::make_unique<Terminal>(
            scheduler, this, io_addr, n, UI_SCREEN_2236DE, vp_mode
        );
#else
        // In terminal server mode, terminals are managed via sessions
        dbglog("IoCardTermMux: Terminal %d available for session connection in terminal server mode\n", n);
#endif
    }
}


// instance destructor
IoCardTermMux::~IoCardTermMux()
{
    if (m_slot >= 0) {
        // not just a temp object, so clean up
        i8080_destroy(static_cast<i8080*>(m_i8080));
        m_i8080 = nullptr;
        for (auto &t : m_terms) {
            // Clean up serial port connection if it exists
            if (t.serial_port) {
                // Flush any pending TX data to prevent spurious output after exit
                t.serial_port->flushTxQueue();
                t.serial_port->detachTerminal();
                t.serial_port->close();
                t.serial_port.reset();
            }
#ifndef HEADLESS_BUILD
            t.terminal = nullptr;
#endif
            t.session.reset();
            // Clear any pending RX data
            t.rx_fifo.clear();
        }
    }
}


std::string
IoCardTermMux::getDescription() const
{
    return "Terminal Mux";
}


std::string
IoCardTermMux::getName() const
{
    return "2236 MXD";
}


// return a list of the various base addresses a card can map to
// the default comes first.
std::vector<int>
IoCardTermMux::getBaseAddresses() const
{
#if 0
    // FIXME: running with more than one MXD causes MVP OS to hang, for reasons
    // I have yet to debug.  having more than one MXD is unwieldy anyway.
    std::vector<int> v { 0x00, 0x40, 0x80, 0xc0 };
#else
    std::vector<int> v { 0x00, };
#endif
    return v;
}


// return the list of addresses that this specific card responds to
std::vector<int>
IoCardTermMux::getAddresses() const
{
    std::vector<int> v;
    for (int i=1; i < 8; i++) {
        v.push_back(m_base_addr + i);
    }
    return v;
}

// -----------------------------------------------------
// configuration management
// -----------------------------------------------------

// subclass returns its own type of configuration object
std::shared_ptr<CardCfgState>
IoCardTermMux::getCfgState()
{
    return std::make_unique<TermMuxCfgState>();
}


// modify the existing configuration state
void
IoCardTermMux::setConfiguration(const CardCfgState &cfg) noexcept
{
    const TermMuxCfgState &ccfg(dynamic_cast<const TermMuxCfgState&>(cfg));

    // FIXME: do sanity checking to make sure things don't change at a bad time?
    //        perhaps queue this change until the next WAKEUP phase?
    m_cfg = ccfg;
};


// -----------------------------------------------------
// operational
// -----------------------------------------------------

// the MXD card has its own power-on-reset circuit. all !PRMS (prime reset)
// does is set a latch that the 8080 can sample.  the latch is cleared
// via "OUT 0".
// interestingly, the reset pin on the i8251 uart (pin 21) is tied low
// i.e., it doesn't have a hard reset.
void
IoCardTermMux::reset(bool /*hard_reset*/) noexcept
{
    m_prime_seen = true;
}


void
IoCardTermMux::select()
{
    m_io_offset = (m_cpu->getAB() & 7);

    if (do_dbg) {
        dbglog("TermMux/%02x +ABS %02x\n", m_base_addr, m_base_addr+m_io_offset);
    }

    // offset 0 is not handled
    if (m_io_offset == 0) {
        return;
    }
    m_selected = true;

    updateRbi();
}


void
IoCardTermMux::deselect()
{
    if (do_dbg) {
        dbglog("TermMux/%02x -ABS %02x\n", m_base_addr, m_base_addr+m_io_offset);
    }
    m_cpu->setDevRdy(false);

    m_selected = false;
    m_cpb      = true;
}


void
IoCardTermMux::strobeOBS(int val)
{
    val &= 0xFF;
    if (do_dbg) {
        dbglog("TermMux/%02x OBS: byte 0x%02x\n", m_base_addr, val);
    }

    // any previous obs or cbs should have been serviced before we see another
    assert(!m_obs_seen && !m_cbs_seen);

    // the hardware latches m_io_offset into another latch on the falling
    // edge of !CBS or !OBS.  I believe the reason is that say the board is
    // addressed at offset 6.  Then it does an OBS(0Xwhatever) in some fire and
    // forget command.  It may take a while to process that OBS, but in the
    // meantime, the host computer may re-address the board at, say, offset 2.
    m_obs_seen = true;
    m_obscbs_offset = m_io_offset;
    m_obscbs_data = val;

    updateRbi();
}


void
IoCardTermMux::strobeCBS(int val)
{
    val &= 0xFF;
    if (do_dbg) {
        dbglog("TermMux/%02x CBS: byte 0x%02x\n", m_base_addr, val);
    }

    // any previous obs or cbs should have been serviced before we see another
    assert(!m_obs_seen && !m_cbs_seen);

    m_cbs_seen = true;
    m_obscbs_offset = m_io_offset;  // secondary address offset latch
    m_obscbs_data = val;

    updateRbi();
}


// weird hack Wang used to signal the attached display is 64x16 (false)
// or 80x24 (true).  All smart terminals are 80x24, but in boot mode/vp mode,
// the term mux looks like a dumb terminal at 05, so it drives this to let
// the ucode know it is 80x24.
int
IoCardTermMux::getIB() const noexcept
{
    // TBD: do we need to give more status than this?
    // In the real hardware, IB is driven by the most recent
    // OUT_IB_N data any time the board is selected.  In addition,
    // any time the address offset is 5 or 7, a gate forcibly drives
    // !IB5 low (logically, the byte is or'd with 0x10). Looking at
    // the MVP microcode, it only ever looks at bit 5.  However, the
    // "CIO SRS" command is exposed via $GIO 760r (Status Request Strobe).
    return (m_io_offset == 5) ? 0x10 : 0x00;
}


// change of CPU Busy state
void
IoCardTermMux::setCpuBusy(bool busy)
{
    // it appears that except for reset, ucode only ever clears it,
    // and of course the IBS sets it back.
    if (do_dbg) {
        dbglog("TermMux/%02x CPB%c\n", m_base_addr, busy ? '+' : '-');
    }
    m_cpb = busy;
}


// perform on instruction and return the number of ns of elapsed time.
int
IoCardTermMux::execOneOp() noexcept
{
    if (m_interrupt_pending) {
        // vector to 0x0038 (rst 7)
        i8080_interrupt(static_cast<i8080*>(m_i8080), 0xFF);
    }

    const int ticks = i8080_exec_one_op(static_cast<i8080*>(m_i8080));
    if (ticks > 30) {
        // it is in an error state
        return 4 * NS_PER_TICK;
    }
    return ticks * NS_PER_TICK;
}


// update the board's !ready/busy status (if selected)
void
IoCardTermMux::updateRbi() noexcept
{
    // don't drive !rbi if the board isn't selected
    if (m_io_offset == 0 || !m_selected) {
        return;
    }

    const bool busy = ((m_obs_seen || m_cbs_seen) && (m_io_offset >= 4))
                   || (((m_rbi >> (m_io_offset-1)) & 1) != 0);

    m_cpu->setDevRdy(!busy);
}


void
IoCardTermMux::updateInterrupt() noexcept
{
    // More efficient interrupt check - exit early on first non-empty FIFO
    bool has_rx_data = false;
    for (int i = 0; i < m_num_terms && !has_rx_data; ++i) {
        has_rx_data = !m_terms[i].rx_fifo.empty();
    }
    m_interrupt_pending = has_rx_data;
}


// a character has come in from the GUI keyboard
void
IoCardTermMux::receiveKeystroke(int term_num, int keycode)
{
    assert((0 <= term_num) && (term_num < MAX_TERMINALS));
    m_term_t &term = m_terms[term_num];

    // If this terminal is connected to a COM port, ignore GUI keystrokes
    // The physical terminal owns the input
    if (term.serial_port) {
        return;
    }

    // Use the new FIFO-based approach for GUI terminals too (consistency)
    queueRxByte(term_num, static_cast<uint8_t>(keycode));
}


void
IoCardTermMux::checkTxBuffer(int term_num)
{
    assert((0 <= term_num) && (term_num < MAX_TERMINALS));
    m_term_t &term = m_terms[term_num];

    if (term.tx_ready || term.tx_tmr) {
        // nothing to do or serial channel is in use
        return;
    }

    term.tx_tmr = m_scheduler->createTimer(
                      SERIAL_CHAR_DELAY,
                      std::bind(&IoCardTermMux::mxdToTermCallback, this, term_num, term.tx_byte)
                  );

    // CRITICAL FIX: Do NOT set tx_ready = true here!
    // tx_ready should only be set after actual transmission completes in mxdToTermCallback()
    // This creates proper backpressure at the Wang CPU level
}


// this causes a delay of 1/char_time before posting a byte to the terminal.
// more than the latency, it is intended to rate limit the channel to match
// that of a real serial terminal.
void
IoCardTermMux::mxdToTermCallback(int term_num, int byte)
{
    assert((0 <= term_num) && (term_num < MAX_TERMINALS));
    m_term_t &term = m_terms[term_num];
    term.tx_tmr = nullptr;

    // Check TX queue backpressure before sending - use lighter approach to avoid RX interference
    if (term.serial_port && term.serial_port->isOpen()) {
        size_t queue_size = term.serial_port->getTxQueueSize();
        size_t queue_capacity = term.serial_port->getTxQueueCapacity();
        float queue_fullness = static_cast<float>(queue_size) / queue_capacity;
        
        // Apply much gentler backpressure to avoid affecting RX responsiveness
        if (queue_fullness > 0.90f) { // Increase threshold to 90% to reduce interference
            // Use much shorter delays: 90%=50μs, 95%=100μs, 100%=200μs
            int64 delay_us = 50 + static_cast<int64>((queue_fullness - 0.90f) * 1500); // 50μs to 200μs max
            term.tx_tmr = m_scheduler->createTimer(
                TIMER_US(delay_us),
                std::bind(&IoCardTermMux::mxdToTermCallback, this, term_num, byte)
            );
            dbglog("IoCardTermMux: TX queue %d%% full (%zu/%zu), delaying %lldμs for terminal %d\n", 
                   static_cast<int>(queue_fullness * 100), queue_size, queue_capacity, delay_us, term_num);
            return;  // Don't proceed with checkTxBuffer yet
        }
    }

    // Route output to appropriate backend: session, serial port, or GUI terminal
    if (term.session) {
        // Send to terminal via session abstraction (preferred for terminal server mode)
        term.session->mxdToTerm(static_cast<uint8>(byte));
    } else if (term.serial_port) {
        // Send to physical terminal via COM port (legacy mode)
        term.serial_port->sendByte(static_cast<uint8>(byte));
    }
#ifndef HEADLESS_BUILD
    else if (term.terminal) {
        // Send to GUI terminal (desktop mode)
        term.terminal->processChar(static_cast<uint8>(byte));
    }
#endif
    
    // CRITICAL FIX: Set tx_ready = true ONLY after successful transmission
    // This creates proper UART timing simulation and prevents CPU flooding
    term.tx_ready = true;
    
    checkTxBuffer(term_num);
}


// Feed serial RX bytes back into the MXD UART (legacy method - kept for compatibility)
void
IoCardTermMux::serialToMxdRx(int term_num, uint8 byte)
{
    // Use the new FIFO-based approach instead of the old single-byte latch
    queueRxByte(term_num, byte);
}


// New FIFO-based RX methods for reliable multi-byte key sequences
void
IoCardTermMux::queueRxByte(int term_num, uint8_t byte)
{
    assert((0 <= term_num) && (term_num < MAX_TERMINALS));
    auto &t = m_terms[term_num];
    
    // Filter out flow control characters that shouldn't reach the emulator
    // XON (0x11/DC1) and XOFF (0x13/DC3) are handled by the serial port layer
    if (byte == 0x11 || byte == 0x13) {
        dbglog("IoCardTermMux: Filtering flow control byte 0x%02X from terminal %d\n", byte, term_num);
        return; // Don't queue flow control characters
    }
    
    if (t.rx_fifo.size() >= RX_FIFO_MAX) {
        // Drop oldest to avoid hard-stall; count a stat
        t.rx_fifo.pop_front();
        ++t.rx_overrun_drops;
    }
    t.rx_fifo.push_back(byte);

    // Send XOFF immediately if buffer becomes full to prevent further overrun
    if (t.rx_fifo.size() >= RX_FIFO_XOFF_THRESHOLD && !t.xoff_sent) {
        sendXOFF(term_num);
    }

    // This should assert the card's RxRDY/interrupt state like the prior rx_ready did
    updateInterrupt();
}

// Batch processing for high-throughput scenarios
void
IoCardTermMux::queueRxBytes(int term_num, const uint8_t* data, size_t length)
{
    assert((0 <= term_num) && (term_num < MAX_TERMINALS));
    if (length == 0) return;
    
    auto &t = m_terms[term_num];
    
    // Calculate how many bytes we can add without overflow
    size_t available_space = (t.rx_fifo.size() < RX_FIFO_MAX) ? 
                            (RX_FIFO_MAX - t.rx_fifo.size()) : 0;
    
    if (available_space == 0) {
        // FIFO is full, drop oldest bytes to make room
        size_t bytes_to_drop = std::min(length, RX_FIFO_MAX / 2);  // Drop up to half the FIFO
        for (size_t i = 0; i < bytes_to_drop; ++i) {
            if (!t.rx_fifo.empty()) {
                t.rx_fifo.pop_front();
                ++t.rx_overrun_drops;
            }
        }
        available_space = RX_FIFO_MAX - t.rx_fifo.size();
    }
    
    // Add as many bytes as we can fit
    size_t bytes_to_add = std::min(length, available_space);
    for (size_t i = 0; i < bytes_to_add; ++i) {
        t.rx_fifo.push_back(data[i]);
    }
    
    // Count any dropped bytes
    if (bytes_to_add < length) {
        t.rx_overrun_drops += (length - bytes_to_add);
    }

    // Update interrupt state once at the end for efficiency
    updateInterrupt();
}


void
IoCardTermMux::serialRxByte(int term_num, uint8_t byte)
{
    // Raw Wang terminal bytes from 2336DW arrive here. Do NOT VT/ANSI-translate.
    // Normalize CR/LF if your system requires (usually leave as-is; CR=0x0D).
    // if (byte == 0x0A) return; // example: drop bare LF if you previously did that

    // RX processing has highest priority - never let TX operations interfere
    queueRxByte(term_num, byte);
    
    // Only check RX-related flow control (XON when buffer drains) - deferred to avoid latency
    // XOFF will be sent when buffer gets full during queueRxByte processing
    if (m_terms[term_num].rx_fifo.size() <= RX_FIFO_XON_THRESHOLD && m_terms[term_num].xoff_sent) {
        checkAndSendFlowControl(term_num);
    }
}

// Check RX FIFO level and send appropriate flow control
void
IoCardTermMux::checkAndSendFlowControl(int term_num)
{
    assert((0 <= term_num) && (term_num < MAX_TERMINALS));
    auto &t = m_terms[term_num];
    
    size_t fifo_size = t.rx_fifo.size();
    
    // Send XOFF if FIFO is getting full and we haven't sent XOFF yet
    if (fifo_size >= RX_FIFO_XOFF_THRESHOLD && !t.xoff_sent) {
        sendXOFF(term_num);
    }
    // Send XON if FIFO has drained enough and we previously sent XOFF
    else if (fifo_size <= RX_FIFO_XON_THRESHOLD && t.xoff_sent) {
        sendXON(term_num);
    }
}

// Send XON to terminal via serial port
void
IoCardTermMux::sendXON(int term_num)
{
    assert((0 <= term_num) && (term_num < MAX_TERMINALS));
    auto &t = m_terms[term_num];
    
    // Send XON via serial port if available
    if (t.serial_port && t.serial_port->isOpen()) {
        t.serial_port->sendXON();
        t.xoff_sent = false;
        t.xon_sent_count++;
        dbglog("IoCardTermMux: Sent XON to terminal %d (FIFO size: %zu)\n", 
               term_num, t.rx_fifo.size());
    }
    // Send XON via session if available  
    else if (t.session && t.session->isActive()) {
        t.session->mxdToTerm(0x11); // DC1 (XON)
        t.xoff_sent = false;
        t.xon_sent_count++;
        dbglog("IoCardTermMux: Sent XON to terminal %d via session (FIFO size: %zu)\n", 
               term_num, t.rx_fifo.size());
    }
}

// Send XOFF to terminal via serial port
void
IoCardTermMux::sendXOFF(int term_num)
{
    assert((0 <= term_num) && (term_num < MAX_TERMINALS));
    auto &t = m_terms[term_num];
    
    // Send XOFF via serial port if available
    if (t.serial_port && t.serial_port->isOpen()) {
        t.serial_port->sendXOFF();
        t.xoff_sent = true;
        t.xoff_sent_count++;
        dbglog("IoCardTermMux: Sent XOFF to terminal %d (FIFO size: %zu)\n", 
               term_num, t.rx_fifo.size());
    }
    // Send XOFF via session if available
    else if (t.session && t.session->isActive()) {
        t.session->mxdToTerm(0x13); // DC3 (XOFF)
        t.xoff_sent = true;
        t.xoff_sent_count++;
        dbglog("IoCardTermMux: Sent XOFF to terminal %d via session (FIFO size: %zu)\n", 
               term_num, t.rx_fifo.size());
    }
}

// Get flow control statistics for debugging/monitoring
void
IoCardTermMux::getFlowControlStats(int term_num, uint32_t* rx_overrun_drops, 
                                  uint64_t* xon_sent_count, uint64_t* xoff_sent_count, 
                                  size_t* fifo_size, bool* xoff_sent) const
{
    assert((0 <= term_num) && (term_num < MAX_TERMINALS));
    const auto &t = m_terms[term_num];
    
    if (rx_overrun_drops) *rx_overrun_drops = t.rx_overrun_drops;
    if (xon_sent_count) *xon_sent_count = t.xon_sent_count;
    if (xoff_sent_count) *xoff_sent_count = t.xoff_sent_count;
    if (fifo_size) *fifo_size = t.rx_fifo.size();
    if (xoff_sent) *xoff_sent = t.xoff_sent;
}

// ============================================================================
// Session management for headless terminal server mode
// ============================================================================

void
IoCardTermMux::setSession(int term_num, std::shared_ptr<ITermSession> session)
{
    assert((0 <= term_num) && (term_num < MAX_TERMINALS));
    
    m_term_t &term = m_terms[term_num];
    
    // Clean up existing connections
    if (term.serial_port) {
        term.serial_port->setReceiveCallback(nullptr);
        term.serial_port->close();
        term.serial_port.reset();
    }
#ifndef HEADLESS_BUILD
    term.terminal.reset();
#endif
    
    // Set the new session
    term.session = session;
    
    if (session) {
        dbglog("IoCardTermMux: Terminal %d connected to session: %s\n", 
               term_num, session->getDescription().c_str());
    } else {
        dbglog("IoCardTermMux: Terminal %d session disconnected\n", term_num);
    }
}

// ============================================================================
// i8080 CPU modeling
// ============================================================================

uint8
IoCardTermMux::i8080_rd_func(int addr, void *user_data) noexcept
{
    if (addr < 0x1000) {
        // read 4K eprom
        return mxd_eprom[addr & 0x0FFF];
    }

    if ((0x2000 <= addr) && (addr < 0x3000)) {
        // read 4KB ram
        const IoCardTermMux *tthis = static_cast<IoCardTermMux*>(user_data);
        assert(tthis != nullptr);
        return tthis->m_ram[addr & 0x0FFF];
    }

    assert(false);
    return 0x00;
}


void
IoCardTermMux::i8080_wr_func(int addr, int byte, void *user_data) noexcept
{
    assert(byte == (byte & 0xff));

    if ((0x2000 <= addr) && (addr < 0x3000)) {
        // write 4KB ram
        IoCardTermMux *tthis = static_cast<IoCardTermMux*>(user_data);
        assert(tthis != nullptr);
        tthis->m_ram[addr & 0x0FFF] = static_cast<uint8>(byte);
        return;
    }
    assert(false);
}


uint8
IoCardTermMux::i8080_in_func(int addr, void *user_data) noexcept
{
    IoCardTermMux *tthis = static_cast<IoCardTermMux*>(user_data);
    assert(tthis != nullptr);
    int term_num = tthis->m_uart_sel;
    m_term_t &term = tthis->m_terms[term_num];

    uint8 rv = 0x00;
    switch (addr) {

    case IN_UART_TXRDY:
        // the hardware inverts the status
        rv = (tthis->m_terms[3].tx_ready ? 0x00 : 0x08)
           | (tthis->m_terms[2].tx_ready ? 0x00 : 0x04)
           | (tthis->m_terms[1].tx_ready ? 0x00 : 0x02)
           | (tthis->m_terms[0].tx_ready ? 0x00 : 0x01);
        break;

    case IN_2200_STATUS:
        {
        const bool cpu_waiting = tthis->m_selected && !tthis->m_cpb;  // CPU waiting for input
        const uint8 msbs = static_cast<uint8>(tthis->m_io_offset << 5);
        rv = (tthis->m_obs_seen   ? 0x01 : 0x00)  // [0]
           | (tthis->m_cbs_seen   ? 0x02 : 0x00)  // [1]
           | (tthis->m_prime_seen ? 0x04 : 0x00)  // [2]
           | (cpu_waiting         ? 0x08 : 0x00)  // [3]
           | (tthis->m_selected   ? 0x10 : 0x00)  // [4]
           | msbs;                                // [7:5]
        }
        break;

    // the 8080 sees the inverted bus polarity
    case IN_OBUS_N:
        tthis->m_obs_seen = false;
        tthis->m_cbs_seen = false;
        tthis->updateRbi();
        rv = (~tthis->m_obscbs_data) & 0xff;
        break;

    case IN_OBSCBS_ADDR:
        {
        const uint8 msbs = static_cast<uint8>(tthis->m_obscbs_offset << 5);
        rv = msbs;  // bits [7:5]
        }
        break;

    case IN_UART_RXRDY:
        rv = (!tthis->m_terms[3].rx_fifo.empty() ? 0x08 : 0x00)
           | (!tthis->m_terms[2].rx_fifo.empty() ? 0x04 : 0x00)
           | (!tthis->m_terms[1].rx_fifo.empty() ? 0x02 : 0x00)
           | (!tthis->m_terms[0].rx_fifo.empty() ? 0x01 : 0x00);
        break;

    case IN_UART_DATA:
        if (!term.rx_fifo.empty()) {
            rv = term.rx_fifo.front();
            term.rx_fifo.pop_front();
            // Check if we should send XON now that we've freed up space
            tthis->checkAndSendFlowControl(term_num);
        } else {
            rv = 0x00;  // Return 0 if FIFO is empty
        }
        // After consuming, update status/IRQ
        tthis->updateInterrupt();
        break;

    case IN_UART_STATUS:
        {
        const bool tx_empty = term.tx_ready && !term.tx_tmr;
        const bool rx_ready = !term.rx_fifo.empty();
        const bool dsr = (term_num < tthis->m_num_terms);
        rv = (term.tx_ready ? 0x01 : 0x00)  // [0] = tx fifo empty
           | (rx_ready      ? 0x02 : 0x00)  // [1] = rx fifo has a byte
           | (tx_empty      ? 0x04 : 0x00)  // [2] = tx serializer and fifo empty
           | (false         ? 0x08 : 0x00)  // [3] = parity error
           | (false         ? 0x10 : 0x00)  // [4] = overrun error
           | (false         ? 0x20 : 0x00)  // [5] = framing error
           | (false         ? 0x40 : 0x00)  // [6] = break detect
           | (dsr           ? 0x80 : 0x00); // [7] = data set ready
        }
        break;

    default:
        assert(false);
    }

    return rv;
}


void
IoCardTermMux::i8080_out_func(int addr, int byte, void *user_data)
{
    IoCardTermMux *tthis = static_cast<IoCardTermMux*>(user_data);
    assert(tthis != nullptr);
    assert(byte == (byte & 0xff));

    switch (addr) {

    case OUT_CLR_PRIME:
        tthis->m_prime_seen = false;
        break;

    case OUT_IB_N:
        byte = (~byte & 0xff);
        if (do_dbg) {
            dbglog("TermMux/%02x IB=%02x\n", tthis->m_base_addr, byte);
        }
        tthis->m_cpu->ioCardCbIbs(byte);
        break;

    case OUT_IB9_N:
        byte = (~byte & 0xff);
        if (do_dbg) {
            dbglog("TermMux/%02x IB=%03x\n", tthis->m_base_addr, 0x100 | byte);
        }
        tthis->m_cpu->ioCardCbIbs(0x100 | byte);
        break;

    case OUT_PRIME:
        // Issue (warm) reset.
        // The real hardware triggers a one shot which drives PRIME active for
        //     R=330K & C=470pf = (0.34ns)*Rx*Cx*(1+1/Rx) = 
        //                      = 0.34*33*470*(1+1/33) = 5000ns = 5ms
        // but it shouldn't matter for this emulation.
        system2200::reset(false);
        break;

    case OUT_HALT_STEP:
        tthis->m_cpu->halt();
        break;

    case OUT_UART_SEL:
        assert(byte == 0x00 || byte == 0x01 || byte == 0x02 || byte == 0x04 || byte == 0x08);
        tthis->m_uart_sel = (byte == 0x01) ? 0
                          : (byte == 0x02) ? 1
                          : (byte == 0x04) ? 2
                          : (byte == 0x08) ? 3
                                           : 0;
        break;

    case OUT_UART_DATA:
        if (tthis->m_uart_sel < tthis->m_num_terms) {
            int term_num   = tthis->m_uart_sel;
            m_term_t &term = tthis->m_terms[term_num];
#if defined(_DEBUG)
            if (!term.tx_ready) {
                UI_warn("terminal %d mxd overwrote the uart tx buffer", term_num+1);
            }
#endif
            term.tx_ready = false;
            term.tx_byte  = byte;
            tthis->checkTxBuffer(term_num);
        }
        break;

    case OUT_UART_CMD:
        // this code emulates only those bits of 8251 functionality which the
        // MXD actually uses, and everything else is assumed to be configured
        // as the MXD configures the UARTs. nobody reading this should use this
        // code as a reference for 8251 behavior!
        // FIXME: it might be good to model overrun, in which case this code
        //        must reset the overrun bit when the clear command is given
        break;

    case OUT_RBI:
        tthis->m_rbi = byte;
        tthis->updateRbi();
        break;
    }
}

// vim: ts=8:et:sw=4:smarttab
