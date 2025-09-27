// ============================================================================
// main_headless.cpp - Wang 2200 Terminal Server
// 
// Multi-port terminal server implementation that connects physical Wang terminals
// via USB-serial adapters to the emulated Wang 2200 system.
// ============================================================================

#include "../../core/system/system2200.h"
#include "../../platform/common/host.h"
#include "../terminal/TerminalServerConfig.h"
#include "../../platform/common/SerialPort.h"
#include "../session/SerialTermSession.h"
#include "../../core/io/IoCardTermMux.h"
#include "../../core/io/IoCard.h"
#include "../../core/system/Scheduler.h"
#include "../terminal/WebConfigServer.h"
#include "../../shared/config/SysCfgState.h"
#include <iostream>
#include <csignal>
#include <chrono>
#include <thread>
#include <memory>
#include <vector>
#include <fstream>
#include <map>
#include <mutex>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <poll.h>
#include <errno.h>
#include <cstring>

// Global state for graceful shutdown
static volatile bool running = true;
static volatile bool dumpStatus = false;
static volatile bool internalRestartRequested = false;
static std::vector<std::shared_ptr<SerialTermSession>> sessions;
static IoCardTermMux* termMux = nullptr;
#ifndef DISABLE_WEBCONFIG
static std::unique_ptr<WebConfigServer> webServer;
#endif

// Function to request internal restart from web server (thread-safe)
void requestInternalRestart() {
    internalRestartRequested = true;
}

// Signal handler for graceful shutdown
void signalHandler(int signal) {
    if (signal == SIGUSR1) {
        dumpStatus = true;
    } else {
        std::cerr << "\n[INFO] Received signal " << signal << ", shutting down gracefully...\n";
        running = false;
        
        // Save configuration immediately in case of crash
        try {
            system2200::cleanup();
            host::terminate();
        } catch (...) {
            // Ignore cleanup errors during signal handling
        }
        
        if (signal == SIGTERM || signal == SIGINT) {
            exit(0);
        }
    }
}

// Generate runtime JSON status with statistics
void outputRuntimeStatus() {
    std::cout << "{" << std::endl;
    std::cout << "  \"timestamp\":" << std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() << "," << std::endl;
    std::cout << "  \"status\":\"running\"," << std::endl;
    std::cout << "  \"terminals\":[" << std::endl;
    
    bool first = true;
    for (size_t i = 0; i < sessions.size(); ++i) {
        if (!first) std::cout << "," << std::endl;
        first = false;
        
        std::cout << "    {\"id\":" << i;
        if (sessions[i] && sessions[i]->isActive()) {
            uint64_t rxBytes, txBytes;
            sessions[i]->getStats(&rxBytes, &txBytes);
            std::cout << ",\"active\":true";
            std::cout << ",\"rx_bytes\":" << rxBytes;
            std::cout << ",\"tx_bytes\":" << txBytes;
            std::cout << ",\"description\":\"" << sessions[i]->getDescription() << "\"";
        } else {
            std::cout << ",\"active\":false";
        }
        std::cout << "}";
    }
    
    std::cout << std::endl << "  ]" << std::endl;
    std::cout << "}" << std::endl;
    std::cout.flush();
}

// Terminal → MXD callback factory
std::function<void(uint8)> createTermToMxdCallback(int termNum) {
    return [termNum](uint8 byte) {
        if (termMux) {
            termMux->serialRxByte(termNum, byte);
        }
    };
}

// Capture callback factory for debugging
std::function<void(uint8, bool)> createCaptureCallback(int termNum, const std::string& captureDir) {
    return [termNum, captureDir](uint8 byte, bool isRx) {
        static std::map<int, std::shared_ptr<std::ofstream>> rxFiles;
        static std::map<int, std::shared_ptr<std::ofstream>> txFiles;
        static std::mutex fileMutex;
        
        std::lock_guard<std::mutex> lock(fileMutex);
        
        auto& fileMap = isRx ? rxFiles : txFiles;
        const char* suffix = isRx ? "rx" : "tx";
        
        if (fileMap.find(termNum) == fileMap.end()) {
            std::string filename = captureDir + "/term" + std::to_string(termNum) + "_" + suffix + ".log";
            auto file = std::make_shared<std::ofstream>(filename, std::ios::binary | std::ios::app);
            if (file->is_open()) {
                fileMap[termNum] = file;
            }
        }
        
        if (fileMap[termNum] && fileMap[termNum]->is_open()) {
            fileMap[termNum]->write(reinterpret_cast<const char*>(&byte), 1);
            fileMap[termNum]->flush();
        }
    };
}

int main(int argc, char* argv[]) {
    std::cerr << "[INFO] Wang 2200 Terminal Server v1.0\n";
    
    // Parse configuration
    TerminalServerConfig config;
    
    try {
        // Load from host config (INI-style) first
        host::initialize();
        
        // First parse command line to check for --ini= argument
        if (!config.parseCommandLine(argc, argv)) {
            return config.shouldExitCleanly() ? 0 : 1; // Clean exit for help/status
        }
        
        // Load from specific INI file if provided, otherwise use default
        if (!config.iniPath.empty()) {
            host::loadConfigFile(config.iniPath);
        }
        
        // Load configuration from host system (may have been loaded from custom INI)
        config.loadFromHostConfig();
        
        // Validate configuration
        if (!config.validate()) {
            return 1;
        }
        
        config.printSummary();
        
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Configuration error: " << e.what() << "\n";
        return 1;
    }
    
    // Set up signal handlers for graceful shutdown
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    signal(SIGUSR1, signalHandler);  // For runtime status output
    
    // Track what we've successfully initialized for safe cleanup
    bool system2200_initialized = false;
    
    try {
        // Initialize the system2200 emulator core
        std::cerr << "[INFO] Initializing Wang 2200 emulator...\n";
        system2200::initialize();
        system2200_initialized = true;
        
        // Find the MXD card at the configured address
        // Note: MXD cards claim addresses base_addr+1 to base_addr+7, not base_addr itself
        std::cerr << "[INFO] Looking for MXD card at base address 0x" << std::hex 
                  << config.mxdIoAddr << std::dec << "...\n";
        
        IoCard* mxdCard = system2200::getInstFromIoAddr(config.mxdIoAddr + 1);
        if (!mxdCard) {
            std::cerr << "[ERROR] No I/O card found at address 0x" << std::hex 
                      << (config.mxdIoAddr + 1) << std::dec << " (base+1)\n";
            return 1;
        }
        
        termMux = dynamic_cast<IoCardTermMux*>(mxdCard);
        if (!termMux) {
            std::cerr << "[ERROR] Card at address 0x" << std::hex << config.mxdIoAddr 
                      << std::dec << " is not a Terminal Multiplexer\n";
            return 1;
        }
        
        std::cerr << "[INFO] Found MXD Terminal Multiplexer card\n";
        
        // Create and configure terminal sessions
        sessions.resize(config.numTerminals);
        
        // Debug: Print terminal configuration
        std::cerr << "[DEBUG] Terminal server configuration:\n";
        for (int i = 0; i < config.numTerminals; i++) {
            std::cerr << "[DEBUG]   Terminal " << i << ": port='" << config.terminals[i].portName 
                      << "' enabled=" << (config.terminals[i].enabled ? "true" : "false") << "\n";
        }
        
        for (int i = 0; i < config.numTerminals; i++) {
            // Skip if terminal has no port configured
            if (config.terminals[i].portName.empty()) {
                std::cerr << "[INFO] Terminal " << i << " has no port configured, skipping\n";
                continue;
            }
            
            if (!config.terminals[i].enabled) {
                std::cerr << "[INFO] Terminal " << i << " disabled in configuration, skipping\n";
                continue;
            }
            
            std::cerr << "[INFO] Setting up terminal " << i << ": " 
                      << config.terminals[i].getDescription() << "\n";
            
            // Check if serial device exists before attempting to open
            const std::string& portName = config.terminals[i].portName;
            if (access(portName.c_str(), F_OK) != 0) {
                std::cerr << "[WARN] Serial device " << portName << " does not exist, terminal " << i << " will be available for later connection\n";
                std::cerr << "[INFO] Check: USB-to-serial adapter connected, permissions (sudo usermod -a -G dialout $USER)\n";
                continue;  // Skip this terminal, don't exit
            }
            
            // Create serial port using the shared scheduler from termMux
            auto scheduler = termMux->getScheduler();
            auto serialPort = std::make_shared<SerialPort>(scheduler);
            
            // Open serial port
            SerialConfig serialConfig = config.terminals[i].toSerialConfig();
            if (!serialPort->open(serialConfig)) {
                std::cerr << "[WARN] Failed to open " << portName << " for terminal " << i << ", will retry later\n";
                std::cerr << "[INFO] Possible causes: device in use, permissions, or hardware issue\n";
                continue;  // Skip this terminal, don't exit
            }
            
            // Set up capture callback if enabled
            if (config.captureEnabled && !config.captureDir.empty()) {
                auto captureCallback = createCaptureCallback(i, config.captureDir);
                serialPort->setCaptureCallback(captureCallback);
                std::cerr << "[INFO] Terminal " << i << " capture enabled to " << config.captureDir << "\n";
            }
            
            // Create session with Terminal → MXD callback
            auto termToMxdCallback = createTermToMxdCallback(i);
            sessions[i] = std::make_shared<SerialTermSession>(serialPort, termToMxdCallback);
            
            // Connect session to MXD
            termMux->setSession(i, sessions[i]);
            
            std::cerr << "[INFO] Terminal " << i << " connected successfully\n";
        }
        
        std::cerr << "[INFO] All terminals configured. Starting emulation...\n";
        
#ifndef DISABLE_WEBCONFIG
        // Start web configuration server if enabled
        if (config.webServerEnabled) {
            std::string iniPath = config.iniPath.empty() ? "wangemu.ini" : config.iniPath;
            webServer = std::make_unique<WebConfigServer>(config.webServerPort, iniPath);


            if (webServer->start()) {
                std::cerr << "[INFO] Web configuration server started on port " << config.webServerPort << "\n";
                std::cerr << "[INFO] Open http://localhost:" << config.webServerPort << " to configure\n";
            } else {
                std::cerr << "[WARN] Failed to start web configuration server\n";
            }
        }
#else
        std::cerr << "[INFO] Web configuration server disabled in this build\n";
#endif
        
        std::cerr << "[INFO] Wang 2200 system ready for terminal connections\n";
        std::cerr << "[INFO] Press Ctrl+C to shutdown gracefully\n";

        // Create timerfd for Option B: unified timer + poll approach
        int timerFd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
        if (timerFd == -1) {
            std::cerr << "[ERROR] Failed to create timerfd: " << strerror(errno) << "\n";
            return 1;
        }

        // Helper lambda to set absolute deadline on timerfd
        auto setTimerDeadline = [timerFd](std::chrono::steady_clock::time_point deadline) {
            using namespace std::chrono;
            auto ns = duration_cast<nanoseconds>(deadline.time_since_epoch()).count();
            itimerspec its{};
            its.it_value.tv_sec = ns / 1000000000LL;
            its.it_value.tv_nsec = ns % 1000000000LL;
            return timerfd_settime(timerFd, TFD_TIMER_ABSTIME, &its, nullptr) == 0;
        };

        // Main emulation loop with timerfd-based sleeping
        using clock = std::chrono::steady_clock;
        auto lastStatsTime = clock::now();
        auto lastRetryTime = clock::now();
        auto nextSlice = clock::now();
        const auto sliceDuration = std::chrono::milliseconds(30); // 30ms slices for good balance

        // Get the scheduler for deadline calculations
        auto scheduler = termMux->getScheduler();

        while (running) {
            // Check for status dump request
            if (dumpStatus) {
                outputRuntimeStatus();
                dumpStatus = false;
            }
            
            // Check for internal restart request (safer than doing it in web handler)
            if (internalRestartRequested) {
                std::cerr << "[INFO] Internal restart requested, performing safe system reconfiguration...\n";
                
                try {
                    // Reload host configuration from file (same as web handler)
                    std::string iniPath = config.iniPath.empty() ? "wangemu.ini" : config.iniPath;
                    host::loadConfigFile(iniPath);
                    std::cerr << "[INFO] Host configuration reloaded from " << iniPath << "\n";
                    
                    // Create new system configuration from host config
                    SysCfgState newConfig;
                    newConfig.loadIni();
                    std::cerr << "[DEBUG] System configuration loaded from host config\n";
                    std::cerr << "[DEBUG] CPU Type: " << newConfig.getCpuType() << "\n";
                    std::cerr << "[DEBUG] RAM Size: " << newConfig.getRamKB() << " KB\n";
                    
                    // Apply the configuration (this is now safe because we're in the main thread)
                    system2200::setConfig(newConfig);
                    std::cerr << "[INFO] System configuration applied - internal restart complete\n";
                    
                } catch (const std::exception& e) {
                    std::cerr << "[ERROR] Internal restart failed: " << e.what() << "\n";
                } catch (...) {
                    std::cerr << "[ERROR] Internal restart failed: unknown error\n";
                }
                
                internalRestartRequested = false;
            }
            
            // Call the core emulator's idle processing
            if (!system2200::onIdle()) {
                break;
            }

            // Calculate next deadline as minimum of:
            // 1. Next fixed time slice (30ms)
            // 2. Next timer expiration
            // 3. Stats/retry intervals (but capped)
            auto now = clock::now();

            // Maintain steady cadence and avoid spin if we fell behind
            // Safety: limit catch-up iterations to prevent infinite loops
            if (nextSlice <= now) {
                int catchupCount = 0;
                do {
                    nextSlice += sliceDuration;
                    catchupCount++;
                    if (catchupCount > 10) { // Safety limit
                        nextSlice = now + sliceDuration;
                        break;
                    }
                } while (nextSlice <= now);
            } else {
                nextSlice += sliceDuration;
            }
            auto deadline = nextSlice;

            // Consider next timer expiration (with minimum interval to prevent busy loops)
            if (auto timerMs = scheduler->getMillisecondsUntilNext()) {
                // Enforce minimum 1ms to prevent zero-timeout busy loops when timers are overdue
                auto safeTimerMs = std::max(*timerMs, static_cast<int64>(1));
                auto timerDeadline = now + std::chrono::milliseconds(safeTimerMs);
                deadline = std::min(deadline, timerDeadline);
            }

            // Consider stats and retry intervals (but don't let them dominate)
            auto statsDeadline = lastStatsTime + std::chrono::seconds(30);
            auto retryDeadline = lastRetryTime + std::chrono::seconds(30);

            // Keep responsive for terminal input - much shorter cap than 500ms
            auto maxDeadline = now + std::chrono::milliseconds(50); // Shorter for terminal responsiveness
            deadline = std::min({deadline, statsDeadline, retryDeadline, maxDeadline});

            // Option B: Use timerfd + ppoll for unified waiting (reduces wakeups)
            if (deadline > now) {
                auto sleepStart = now;

                // Set timerfd to expire at calculated deadline
                if (!setTimerDeadline(deadline)) {
                    std::cerr << "[WARN] Failed to set timerfd deadline: " << strerror(errno) << "\n";
                    // Fallback to sleep_until if timerfd fails
                    std::this_thread::sleep_until(deadline);
                } else {
                    // Use ppoll to wait on timerfd (single wait point for main loop)
                    pollfd pfd = { .fd = timerFd, .events = POLLIN, .revents = 0 };
                    int result = ppoll(&pfd, 1, nullptr, nullptr);

#ifdef DEBUG_WAKEUPS
                    if (result == 0) {
                        std::cerr << "[WAKE] timeout to deadline\n";
                    } else if (result > 0 && (pfd.revents & POLLIN)) {
                        std::cerr << "[WAKE] deadline timer fired\n";
                    }
#endif
                    if (result > 0 && (pfd.revents & POLLIN)) {
                        // Timer expired - acknowledge it by reading the expiration count
                        uint64_t expirations;
                        ssize_t s = read(timerFd, &expirations, sizeof(expirations));
                        (void)s; // Suppress unused variable warning
                    }
                    // If result <= 0, we were interrupted by signal (which is fine)
                }

                auto wakeTime = clock::now();

                // Debug wakeup reasons if enabled
                if (config.debugWakeups) {
                    auto actualSleep = std::chrono::duration_cast<std::chrono::milliseconds>(wakeTime - sleepStart);
                    auto expectedSleep = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - sleepStart);

                    std::string reason = "unknown";
                    if (actualSleep >= expectedSleep - std::chrono::milliseconds(1)) {
                        if (deadline == nextSlice) reason = "time_slice";
                        else if (scheduler->hasPendingTimers()) reason = "timer_expired";
                        else reason = "periodic_maintenance";
                    } else {
                        reason = "early_wake"; // Signal or other interruption
                    }

                    std::cerr << "[DEBUG] Woke after " << actualSleep.count()
                              << "ms (expected " << expectedSleep.count()
                              << "ms), reason: " << reason << " [timerfd]" << std::endl;
                }
            }

            // Print session stats every 30 seconds
            now = clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastStatsTime);
            if (elapsed.count() >= 30) {
                std::cerr << "[INFO] Session stats:\n";
                for (int i = 0; i < config.numTerminals; i++) {
                    if (sessions[i] && sessions[i]->isActive()) {
                        uint64_t rxBytes, txBytes;
                        sessions[i]->getStats(&rxBytes, &txBytes);
                        std::cerr << "[INFO]   Terminal " << i << ": RX=" << rxBytes 
                                  << " TX=" << txBytes << " bytes\n";
                    }
                }
                lastStatsTime = now;
            }
            
            // Try to reconnect failed serial ports every 30 seconds
            auto retryElapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastRetryTime);
            if (retryElapsed.count() >= 30) {
                for (int i = 0; i < config.numTerminals; i++) {
                    if (sessions[i]) {
                        continue;  // Skip already connected terminals
                    }
                    
                    // Skip if terminal has no port configured
                    if (config.terminals[i].portName.empty()) {
                        continue;
                    }
                    
                    const std::string& portName = config.terminals[i].portName;
                    
                    // Check if device now exists
                    if (access(portName.c_str(), F_OK) != 0) {
                        continue;  // Still doesn't exist
                    }
                    
                    std::cerr << "[INFO] Attempting to reconnect terminal " << i << " to " << portName << "\n";
                    
                    // Try to create and open serial port
                    auto scheduler = termMux->getScheduler();
                    auto serialPort = std::make_shared<SerialPort>(scheduler);
                    SerialConfig serialConfig = config.terminals[i].toSerialConfig();
                    
                    if (serialPort->open(serialConfig)) {
                        // Set up capture callback if enabled
                        if (config.captureEnabled && !config.captureDir.empty()) {
                            auto captureCallback = createCaptureCallback(i, config.captureDir);
                            serialPort->setCaptureCallback(captureCallback);
                        }
                        
                        // Create session and connect to MXD
                        auto termToMxdCallback = createTermToMxdCallback(i);
                        sessions[i] = std::make_shared<SerialTermSession>(serialPort, termToMxdCallback);
                        termMux->setSession(i, sessions[i]);
                        
                        std::cerr << "[INFO] Terminal " << i << " reconnected successfully to " << portName << "\n";
                    }
                }
                lastRetryTime = now;
            }
        }
        
        std::cerr << "[INFO] Main loop exited, cleaning up sessions...\n";

        // Clean up timerfd
        if (timerFd != -1) {
            close(timerFd);
            timerFd = -1;
        }

        // Stop web server
#ifndef DISABLE_WEBCONFIG
        if (webServer) {
            std::cerr << "[INFO] Stopping web configuration server...\n";
            webServer->stop();
            webServer.reset();
        }
#endif

        // Clean up sessions
        for (int i = 0; i < config.numTerminals; i++) {
            if (sessions[i]) {
                termMux->setSession(i, nullptr);
                sessions[i].reset();
            }
        }
        
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Runtime error: " << e.what() << "\n";
        // Clean up what we've initialized
        try {
            if (system2200_initialized) {
                system2200::cleanup();
            }
            host::terminate();
        } catch (...) {
            // Ignore cleanup errors during exception handling
        }
        return 1;
    } catch (...) {
        std::cerr << "[ERROR] Unknown exception\n";
        // Clean up what we've initialized
        try {
            if (system2200_initialized) {
                system2200::cleanup();
            }
            host::terminate();
        } catch (...) {
            // Ignore cleanup errors during exception handling
        }
        return 1;
    }
    
    // Clean shutdown
    try {
        if (system2200_initialized) {
            system2200::cleanup();
        }
        host::terminate();
        std::cerr << "[INFO] Shutdown complete\n";
    } catch (...) {
        std::cerr << "[ERROR] Exception during cleanup\n";
        return 1;
    }
    
    return 0;
}