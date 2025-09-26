// Function MXD Terminal Mux card emulation

#ifndef _INCLUDE_IOCARD_TERM_MUX_H_
#define _INCLUDE_IOCARD_TERM_MUX_H_

#include "IoCard.h"
#include "TermMuxCfgState.h"
#include <memory>
#include <deque>

class Cpu2200;
class Scheduler;
class Timer;
#ifndef HEADLESS_BUILD
class Terminal;
#endif
class SerialPort;
struct ITermSession;

class IoCardTermMux : public IoCard
{
public:
    // see the base class for the definition of the public functions
    CANT_ASSIGN_OR_COPY_CLASS(IoCardTermMux);

    // ----- common IoCard functions -----
    IoCardTermMux(std::shared_ptr<Scheduler> scheduler,
                  std::shared_ptr<Cpu2200> cpu,
                  int base_addr, int card_slot,
                  const CardCfgState *cfg);
    ~IoCardTermMux() override;

    std::vector<int> getAddresses() const override;

    void  setConfiguration(const CardCfgState &cfg) noexcept override;

    void  reset(bool hard_reset) noexcept override;
    void  select() override;
    void  deselect() override;
    void  strobeOBS(int val) override;
    void  strobeCBS(int val) override;
    int   getIB() const noexcept override;
    void  setCpuBusy(bool busy) override;

    // a keyboard event has happened
    void receiveKeystroke(int term_num, int keycode);
    
    // Session management for headless terminal server mode
    void setSession(int term_num, std::shared_ptr<ITermSession> session);
    
    // Terminal → MXD data input (public for headless terminal server)
    void serialRxByte(int term_num, uint8_t byte);
    
    // Get shared scheduler for terminal server components
    std::shared_ptr<Scheduler> getScheduler() const { return m_scheduler; }
    
    // Get flow control statistics for debugging
    void getFlowControlStats(int term_num, uint32_t* rx_overrun_drops, 
                            uint64_t* xon_sent_count, uint64_t* xoff_sent_count, 
                            size_t* fifo_size, bool* xoff_sent) const;

private:

    static const int MAX_TERMINALS = 4;

    // ---- card properties ----
    std::string       getDescription() const override;
    std::string       getName() const override;
    std::vector<int>  getBaseAddresses() const override;
    bool              isConfigurable() const noexcept override { return true; }
    std::shared_ptr<CardCfgState> getCfgState() override;

    // i8080 hal interface
    static uint8 i8080_rd_func(int addr, void *user_data) noexcept;
    static void  i8080_wr_func(int addr, int byte, void *user_data) noexcept;
    static uint8 i8080_in_func(int addr, void *user_data) noexcept;
    static void  i8080_out_func(int addr, int byte, void *user_data);

    // perform one i8080 instruction
    int execOneOp() noexcept;

    // update the board's !ready/busy status (if selected)
    void updateRbi() noexcept;

    // raise an interrupt if any uart has an rx char ready
    void updateInterrupt() noexcept;

    void checkTxBuffer(int term_num);
    void mxdToTermCallback(int term_num, int byte);
    
    // Handle bytes received from serial port
    void serialToMxdRx(int term_num, uint8 byte);
    
    // RX FIFO management
    void queueRxByte(int term_num, uint8_t byte);
    void queueRxBytes(int term_num, const uint8_t* data, size_t length);  // Batch processing
    
    // Flow control management
    void checkAndSendFlowControl(int term_num);
    void sendXON(int term_num);
    void sendXOFF(int term_num);
    
    // FIFO capacity - increased from 64 to 2048 for better flow control
    static constexpr size_t RX_FIFO_MAX = 2048;
    
    // Flow control watermarks for XON/XOFF
    static constexpr size_t RX_FIFO_XOFF_THRESHOLD = (RX_FIFO_MAX * 3) / 4;  // 75% - send XOFF
    static constexpr size_t RX_FIFO_XON_THRESHOLD = (RX_FIFO_MAX * 1) / 4;   // 25% - send XON

    // ---- board state ----
    TermMuxCfgState            m_cfg;       // current configuration
    std::shared_ptr<Scheduler> m_scheduler; // shared event scheduler
    std::shared_ptr<Cpu2200>   m_cpu;       // associated CPU
    const int   m_base_addr;         // the address the card is mapped to
    const int   m_slot;              // which slot the card is plugged into
    void       *m_i8080 = nullptr;   // control processor
    uint8       m_ram[4096];         // i8080 RAM

    int  m_num_terms         = 0;     // number of terminals attached to MXD
    bool m_selected          = false; // the card is currently selected
    bool m_cpb               = true;  // the cpu is busy
    int  m_io_offset         = 0;     // (selected io addr & 7), for convenience
    bool m_prime_seen        = true;  // !PRMS (reset) strobe received
    bool m_obs_seen          = false; // OBS strobe received
    bool m_cbs_seen          = false; // CBS strobe received
    int  m_obscbs_offset     = 0;     // io_offset at time of obs or cbs strobe
    int  m_obscbs_data       = 0x00;  // data captured at obs or cbs strobe
    int  m_rbi               = 0xff;  // 0=ready, 1=busy
    int  m_uart_sel          = 0;     // currently addressed uart, 0..3
    bool m_interrupt_pending = false; // one of the uarts has an rx byte

    // ---- per terminal state ----
    struct m_term_t {
        // display related:
#ifndef HEADLESS_BUILD
        std::unique_ptr<Terminal> terminal; // terminal model
#endif
        std::shared_ptr<SerialPort> serial_port; // COM port (if used)
        std::shared_ptr<ITermSession> session; // session abstraction (headless mode)
        // uart receive state (legacy single-byte latch - kept for compatibility)
        bool                   rx_ready;    // received a byte
        int                    rx_byte;     // value of received byte
        // RX FIFO (new implementation)
        std::deque<uint8_t>    rx_fifo;     // RX FIFO for reliable multi-byte sequences
        uint32_t               rx_overrun_drops = 0; // statistics for debugging
        // Flow control state
        bool                   xoff_sent = false;    // true if XOFF has been sent and not cleared by XON
        uint64_t               xoff_sent_count = 0;  // number of times XOFF was sent
        uint64_t               xon_sent_count = 0;   // number of times XON was sent
        // uart transmit state
        bool                   tx_ready;    // room to accept a byte (1 deep FIFO)
        int                    tx_byte;     // value of tx byte
        std::shared_ptr<Timer> tx_tmr;      // model uart rate & delay
    } m_terms[MAX_TERMINALS];
};

#endif // _INCLUDE_IOCARD_TERM_MUX_H_

// vim: ts=8:et:sw=4:smarttab
