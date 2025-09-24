// ============================================================================
// main_headless.cpp - Headless Wang Emulator Terminal Server
// 
// Multi-port terminal server implementation that connects physical Wang terminals
// via USB-serial adapters to the emulated Wang 2200 system.
// ============================================================================

#include "system2200.h"
#include "host.h"
#include "TerminalServerConfig.h"
#include "SerialPort.h"
#include "SerialTermSession.h"
#include "IoCardTermMux.h"
#include "IoCard.h"
#include "Scheduler.h"
#include <iostream>
#include <csignal>
#include <chrono>
#include <thread>
#include <memory>
#include <vector>
#include <fstream>
#include <map>
#include <mutex>

// Global state for graceful shutdown
static volatile bool running = true;
static volatile bool dumpStatus = false;
static std::vector<std::shared_ptr<SerialTermSession>> sessions;
static IoCardTermMux* termMux = nullptr;

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
    std::cerr << "[INFO] Wang 2200 Headless Terminal Server v1.0\n";
    
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
    
    try {
        // Initialize the system2200 emulator core
        std::cerr << "[INFO] Initializing Wang 2200 emulator...\n";
        system2200::initialize();
        
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
        
        for (int i = 0; i < config.numTerminals; i++) {
            if (!config.terminals[i].enabled) {
                std::cerr << "[INFO] Terminal " << i << " disabled, skipping\n";
                continue;
            }
            
            std::cerr << "[INFO] Setting up terminal " << i << ": " 
                      << config.terminals[i].getDescription() << "\n";
            
            // Create serial port using the shared scheduler from termMux
            auto scheduler = termMux->getScheduler();
            auto serialPort = std::make_shared<SerialPort>(scheduler);
            
            // Open serial port
            SerialConfig serialConfig = config.terminals[i].toSerialConfig();
            if (!serialPort->open(serialConfig)) {
                std::cerr << "[ERROR] Failed to open " << config.terminals[i].portName 
                          << " for terminal " << i << "\n";
                return 1;
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
        std::cerr << "[INFO] Wang 2200 system ready for terminal connections\n";
        std::cerr << "[INFO] Press Ctrl+C to shutdown gracefully\n";
        
        // Main emulation loop
        auto lastStatsTime = std::chrono::steady_clock::now();
        while (running) {
            // Check for status dump request
            if (dumpStatus) {
                outputRuntimeStatus();
                dumpStatus = false;
            }
            
            // Call the core emulator's idle processing
            if (!system2200::onIdle()) {
                break;
            }
            
            // Print session stats every 30 seconds
            auto now = std::chrono::steady_clock::now();
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
            
            // Small sleep to prevent 100% CPU usage
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        
        std::cerr << "[INFO] Main loop exited, cleaning up sessions...\n";
        
        // Clean up sessions
        for (int i = 0; i < config.numTerminals; i++) {
            if (sessions[i]) {
                termMux->setSession(i, nullptr);
                sessions[i].reset();
            }
        }
        
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Runtime error: " << e.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "[ERROR] Unknown exception\n";
        return 1;
    }
    
    // Clean shutdown
    try {
        system2200::cleanup();
        host::terminate();
        std::cerr << "[INFO] Shutdown complete\n";
    } catch (...) {
        std::cerr << "[ERROR] Exception during cleanup\n";
        return 1;
    }
    
    return 0;
}