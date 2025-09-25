// ============================================================================
// main_headless.cpp - Wang 2200 Terminal Server
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
#include "WebConfigServer.h"
#include "SysCfgState.h"
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

// Global state for graceful shutdown
static volatile bool running = true;
static volatile bool dumpStatus = false;
static volatile bool internalRestartRequested = false;
static std::vector<std::shared_ptr<SerialTermSession>> sessions;
static IoCardTermMux* termMux = nullptr;
static std::unique_ptr<WebConfigServer> webServer;

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
        
        std::cerr << "[INFO] Wang 2200 system ready for terminal connections\n";
        std::cerr << "[INFO] Press Ctrl+C to shutdown gracefully\n";
        
        // Main emulation loop
        auto lastStatsTime = std::chrono::steady_clock::now();
        auto lastRetryTime = std::chrono::steady_clock::now();
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
            
            // Small sleep to prevent 100% CPU usage
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        
        std::cerr << "[INFO] Main loop exited, cleaning up sessions...\n";
        
        // Stop web server
        if (webServer) {
            std::cerr << "[INFO] Stopping web configuration server...\n";
            webServer->stop();
            webServer.reset();
        }
        
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