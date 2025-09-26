// ============================================================================
// UiHeadless.cpp - Terminal server implementation of UI functions
// 
// This file provides minimal implementations of the UI_* functions
// required by the core emulator when running in terminal server mode.
// ============================================================================

#include "../../gui/system/Ui.h"
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <memory>

// =============================================================
// Terminal server UI implementations
// =============================================================

// Display functions - no-ops for terminal server
std::shared_ptr<CrtFrame>
UI_displayInit(int screen_type, int io_addr, int term_num, crt_state_t *crt_state)
{
    fprintf(stderr, "[INFO] Terminal server: display init for term %d at I/O 0x%03X (screen type %d)\n", 
            term_num, io_addr, screen_type);
    return nullptr;  // terminal server mode doesn't create actual display
}

void UI_displayDestroy(CrtFrame *wnd)
{
    // no-op for terminal server
}

void UI_displayDing(CrtFrame *wnd)
{
    // no-op for terminal server (could print BEL to stderr if desired)
}

// Simulation status
void UI_setSimSeconds(unsigned long seconds, float relative_speed)
{
    // Could log periodically, but usually silent
    static unsigned long last_logged = 0;
    if (seconds - last_logged >= 60) {  // log every minute
        fprintf(stderr, "[INFO] Simulation time: %lu seconds (%.1fx speed)\n", seconds, relative_speed);
        last_logged = seconds;
    }
}

// Disk events
void UI_diskEvent(int slot, int drive)
{
    // no-op for terminal server, or could log
}

// Printer functions - no-ops for terminal server
std::shared_ptr<PrinterFrame> UI_printerInit(int io_addr)
{
    fprintf(stderr, "[INFO] Terminal server: printer init at I/O 0x%03X\n", io_addr);
    return nullptr;
}

void UI_printerDestroy(PrinterFrame *wnd)
{
    // no-op
}

void UI_printerChar(PrinterFrame *wnd, uint8 byte)
{
    // no-op for terminal server (could write to file if needed)
}

// System configuration - no-op
void UI_systemConfigDlg()
{
    fprintf(stderr, "[WARN] Terminal server: system configuration dialog requested but not available\n");
}

void UI_configureCard(IoCard::card_t card_type, CardCfgState *cfg)
{
    fprintf(stderr, "[WARN] Terminal server: card configuration dialog requested but not available\n");
}

// Status/error reporting functions
void UI_error(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[ERROR] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

void UI_warn(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[WARN] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

void UI_info(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[INFO] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

bool UI_confirm(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[CONFIRM] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, " (auto-answered: NO)\n");
    va_end(args);
    return false;  // terminal server mode auto-declines confirmations
}