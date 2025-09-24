// ============================================================================
// host_headless.cpp - Terminal server implementation of host services
// 
// This provides minimal implementations of the host:: functions
// for running in terminal server mode without wxWidgets dependencies.
// Configuration is handled through simple key-value pairs in memory.
// ============================================================================

#include "host.h"
#include <map>
#include <string>
#include <chrono>
#include <thread>
#include <cstdarg>
#include <cstdio>
#include <unistd.h>
#include <cstdlib>
#include <fstream>
#include <sstream>

// In-memory configuration storage - preserving original INI structure
static std::map<std::string, std::map<std::string, std::string>> config_sections;
static std::string ini_filename = "wangemu.ini";

// Helper to create config key for internal flat storage (backward compatibility)
static std::string makeConfigKey(const std::string &subgroup, const std::string &key) {
    return subgroup + "/" + key;
}

// Wang standard I/O address validation
static bool isValidDiskControllerAddress(int addr) {
    return (addr == 0x310 || addr == 0x320 || addr == 0x330);
}

static int getStandardDiskControllerAddress(int configured_addr) {
    // Wang standard: Disk controllers at 0x310, 0x320, or 0x330
    if (isValidDiskControllerAddress(configured_addr)) {
        return configured_addr;
    }
    
    // If not standard, default to 0x310
    if (configured_addr != 0x310) {
        fprintf(stderr, "[WARN] Disk controller configured at 0x%X, correcting to Wang standard 0x310\n", 
                configured_addr);
    }
    return 0x310;
}

// Create default headless configuration
static void createTerminalServerDefaults() {
    // Clear any existing configuration
    config_sections.clear();
    
    // Basic wangemu config
    config_sections["wangemu"]["configversion"] = "1";
    config_sections["wangemu/config-0"] = std::map<std::string, std::string>();
    
    // CPU configuration - VP system
    config_sections["wangemu/config-0/cpu"]["cpu"] = "2200MVP-C";
    config_sections["wangemu/config-0/cpu"]["memsize"] = "512";
    config_sections["wangemu/config-0/cpu"]["speed"] = "regulated";
    
    // Misc settings
    config_sections["wangemu/config-0/misc"]["disk_realtime"] = "true";
    config_sections["wangemu/config-0/misc"]["warnio"] = "true";
    
    // I/O slots - clear all slots first
    for (int slot = 0; slot < 8; slot++) {
        std::string slot_section = "wangemu/config-0/io/slot-" + std::to_string(slot);
        config_sections[slot_section]["type"] = "";
        config_sections[slot_section]["addr"] = "";
    }
    
    // Slot 0: MXD Terminal Multiplexer (Wang standard)
    config_sections["wangemu/config-0/io/slot-0"]["type"] = "2236 MXD";
    config_sections["wangemu/config-0/io/slot-0"]["addr"] = "0x000";
    
    // MXD card configuration - 1 terminal by default
    config_sections["wangemu/config-0/io/slot-0/cardcfg"]["numTerminals"] = "1";
    config_sections["wangemu/config-0/io/slot-0/cardcfg"]["terminal0_com_port"] = "/dev/ttyUSB0";
    config_sections["wangemu/config-0/io/slot-0/cardcfg"]["terminal0_baud_rate"] = "19200";
    config_sections["wangemu/config-0/io/slot-0/cardcfg"]["terminal0_flow_control"] = "0";
    config_sections["wangemu/config-0/io/slot-0/cardcfg"]["terminal0_sw_flow_control"] = "1";
    
    // Clear unused terminals
    for (int term = 1; term < 4; term++) {
        std::string term_prefix = "terminal" + std::to_string(term) + "_";
        config_sections["wangemu/config-0/io/slot-0/cardcfg"][term_prefix + "com_port"] = "";
        config_sections["wangemu/config-0/io/slot-0/cardcfg"][term_prefix + "baud_rate"] = "19200";
        config_sections["wangemu/config-0/io/slot-0/cardcfg"][term_prefix + "flow_control"] = "0";
        config_sections["wangemu/config-0/io/slot-0/cardcfg"][term_prefix + "sw_flow_control"] = "0";
    }
    
    // Slot 1: Disk Controller (Wang standard)
    config_sections["wangemu/config-0/io/slot-1"]["type"] = "6541";
    config_sections["wangemu/config-0/io/slot-1"]["addr"] = "0x310";
    config_sections["wangemu/config-0/io/slot-1"]["filename-0"] = "";
    config_sections["wangemu/config-0/io/slot-1"]["filename-1"] = "";
    
    // Disk controller configuration
    config_sections["wangemu/config-0/io/slot-1/cardcfg"]["numDrives"] = "2";
    config_sections["wangemu/config-0/io/slot-1/cardcfg"]["intelligence"] = "smart";
    config_sections["wangemu/config-0/io/slot-1/cardcfg"]["warnMismatch"] = "true";
    
    // COM terminal config (for compatibility)
    config_sections["wangemu/config-0/com_terminal"]["port_name"] = "/dev/ttyUSB0";
    config_sections["wangemu/config-0/com_terminal"]["baud_rate"] = "19200";
    config_sections["wangemu/config-0/com_terminal"]["flow_control"] = "false";
    config_sections["wangemu/config-0/com_terminal"]["sw_flow_control"] = "true";
    
    fprintf(stderr, "[INFO] Created default terminal server configuration: MXD at slot 0 (0x000), disk at slot 1 (0x310)\n");
}

// INI file parser that preserves original structure  
static void loadIniFile(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        fprintf(stderr, "[DEBUG] Could not open %s\n", filename.c_str());
        return; // File doesn't exist or can't be opened
    }
    
    fprintf(stderr, "[DEBUG] Successfully opened %s\n", filename.c_str());
    
    config_sections.clear(); // Clear existing sections
    
    std::string line;
    std::string current_section;
    
    while (std::getline(file, line)) {
        // Remove leading/trailing whitespace
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);
        
        // Skip empty lines and comments
        if (line.empty() || line[0] == ';' || line[0] == '#') {
            continue;
        }
        
        // Check for section header [section]
        if (line[0] == '[' && line.back() == ']') {
            current_section = line.substr(1, line.length() - 2);
            // Ensure section exists in map
            if (config_sections.find(current_section) == config_sections.end()) {
                config_sections[current_section] = std::map<std::string, std::string>();
            }
            continue;
        }
        
        // Parse key=value pairs
        size_t eq_pos = line.find('=');
        if (eq_pos != std::string::npos && !current_section.empty()) {
            std::string key = line.substr(0, eq_pos);
            std::string value = line.substr(eq_pos + 1);
            
            // Remove whitespace around key and value
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);
            
            // Store in section map
            config_sections[current_section][key] = value;
        }
    }
    
    file.close();
}

// Save configuration to INI file preserving original structure
static void saveIniFile(const std::string& filename) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        fprintf(stderr, "[ERROR] Could not write to %s\n", filename.c_str());
        return;
    }
    
    fprintf(stderr, "[INFO] Saving configuration to %s\n", filename.c_str());
    
    // Write sections in the same order and structure as they were loaded
    for (const auto& section_entry : config_sections) {
        const std::string& section_name = section_entry.first;
        const std::map<std::string, std::string>& keys = section_entry.second;
        
        file << "[" << section_name << "]\n";
        
        // Write keys for this section
        for (const auto& key_entry : keys) {
            file << key_entry.first << "=" << key_entry.second << "\n";
        }
    }
    
    file.close();
}

namespace host
{

void initialize()
{
    fprintf(stderr, "[INFO] Host subsystem initialized (terminal server mode)\n");
    
    // Try to load existing wangemu.ini file if it exists
    loadIniFile(ini_filename);
    
    if (!config_sections.empty()) {
        fprintf(stderr, "[INFO] Loaded configuration from wangemu.ini\n");
    } else {
        fprintf(stderr, "[INFO] No wangemu.ini found, creating terminal server defaults\n");
        createTerminalServerDefaults();
    }
}

void loadConfigFile(const std::string& filename)
{
    fprintf(stderr, "[INFO] Loading configuration from %s\n", filename.c_str());
    config_sections.clear();  // Clear existing config first
    loadIniFile(filename);
    if (!config_sections.empty()) {
        fprintf(stderr, "[INFO] Configuration loaded successfully\n");
    } else {
        fprintf(stderr, "[WARN] No configuration found in %s\n", filename.c_str());
    }
}

void terminate()
{
    if (!config_sections.empty()) {
        saveIniFile(ini_filename);
    }
    config_sections.clear();
}

// ---- Configuration functions ----

bool configReadStr(const std::string &subgroup,
                   const std::string &key,
                   std::string *val,
                   const std::string *defaultval)
{
    // Try to find in proper section format first
    std::string section = "wangemu/config-0/" + subgroup;
    auto section_it = config_sections.find(section);
    if (section_it != config_sections.end()) {
        auto key_it = section_it->second.find(key);
        if (key_it != section_it->second.end()) {
            *val = key_it->second;
            return true;
        }
    }
    
    if (defaultval) {
        *val = *defaultval;
        return true;
    }
    return false;
}

void configWriteStr(const std::string &subgroup,
                    const std::string &key,
                    const std::string &val)
{
    std::string section = "wangemu/config-0/" + subgroup;
    // Ensure section exists
    if (config_sections.find(section) == config_sections.end()) {
        config_sections[section] = std::map<std::string, std::string>();
    }
    config_sections[section][key] = val;
}

bool configReadInt(const std::string &subgroup,
                   const std::string &key,
                   int *val,
                   int defaultval)
{
    // Hard-disable GUI-only devices in terminal server mode
    if (subgroup == "display" &&
        (key == "num_crt" || key == "enable")) {
        *val = 0;
        return true;
    }
    if (subgroup == "printer" && key == "enable") {
        *val = 0;
        return true;
    }
    // Terminal server defaults
    if (subgroup == "terminal_server" && key == "num_terms") {
        *val = 1;
        return true;
    }
    if (subgroup == "terminal_server" && key == "mxd_io_addr") {
        // Wang standard: MXD/MUX cards MUST be at base address 0x000
        // Try to find MXD card in INI configuration first
        for (int slot = 0; slot < 8; slot++) {
            std::string slot_section = "wangemu/config-0/io/slot-" + std::to_string(slot);
            auto slot_section_it = config_sections.find(slot_section);
            
            if (slot_section_it != config_sections.end()) {
                auto type_it = slot_section_it->second.find("type");
                
                if (type_it != slot_section_it->second.end() && type_it->second == "2236 MXD") {
                    // Found MXD card - always force to Wang standard
                    *val = 0x000;  // Force Wang standard addressing
                    fprintf(stderr, "[INFO] Found MXD in slot %d, using Wang standard address 0x000\n", slot);
                    return true;
                }
            }
        }
        
        // If no MXD found in INI, use Wang standard default (this shouldn't happen with defaults)
        fprintf(stderr, "[WARN] No MXD card found in configuration, using Wang standard address 0x000\n");
        *val = 0x000;
        return true;
    }

    // Try to find in proper section format
    std::string section = "wangemu/config-0/" + subgroup;
    auto section_it = config_sections.find(section);
    if (section_it != config_sections.end()) {
        auto key_it = section_it->second.find(key);
        if (key_it != section_it->second.end()) {
            const std::string& value_str = key_it->second;
            // Handle empty values by using default
            if (value_str.empty()) {
                *val = defaultval;
                return false;
            }
            // Handle hex values like 0x310, 0x215, etc.
            if (value_str.length() > 2 && value_str.substr(0, 2) == "0x") {
                std::string hex_part = value_str.substr(2);
                *val = std::stoi(hex_part, nullptr, 16);
            } else {
                *val = std::stoi(value_str, nullptr, 0);
            }
            
            // Wang standard address validation for I/O cards
            if (subgroup.find("io/slot-") == 0 && key == "addr" && *val != 0) {
                // Check if this is a disk controller
                auto type_it = section_it->second.find("type");
                if (type_it != section_it->second.end()) {
                    if (type_it->second == "6541" || type_it->second.find("disk") != std::string::npos) {
                        // This is a disk controller - enforce Wang standards
                        *val = getStandardDiskControllerAddress(*val);
                    }
                }
            }
            
            return true;
        }
    }
    *val = defaultval;
    return false;
}

void configWriteInt(const std::string &subgroup,
                    const std::string &key,
                    int val)
{
    std::string section = "wangemu/config-0/" + subgroup;
    // Ensure section exists
    if (config_sections.find(section) == config_sections.end()) {
        config_sections[section] = std::map<std::string, std::string>();
    }
    config_sections[section][key] = std::to_string(val);
}

void configReadBool(const std::string &subgroup,
                    const std::string &key,
                    bool *val,
                    bool defaultval)
{
    std::string section = "wangemu/config-0/" + subgroup;
    auto section_it = config_sections.find(section);
    if (section_it != config_sections.end()) {
        auto key_it = section_it->second.find(key);
        if (key_it != section_it->second.end()) {
            *val = (key_it->second == "true" || key_it->second == "1");
            return;
        }
    }
    *val = defaultval;
}

void configWriteBool(const std::string &subgroup,
                     const std::string &key,
                     bool val)
{
    std::string section = "wangemu/config-0/" + subgroup;
    // Ensure section exists
    if (config_sections.find(section) == config_sections.end()) {
        config_sections[section] = std::map<std::string, std::string>();
    }
    config_sections[section][key] = val ? "true" : "false";
}

// Forward declarations for terminal server build
class wxWindow;
class wxRect;

// Window geometry functions - no-ops for terminal server
void configReadWinGeom(wxWindow *wxwin,
                       const std::string &subgroup,
                       wxRect *default_geom,
                       bool client_size)
{
    // no-op in terminal server mode
}

void configWriteWinGeom(wxWindow *wxwin,
                        const std::string &subgroup,
                        bool client_size)
{
    // no-op in terminal server mode
}

// ---- Time functions ----

int64 getTimeMs()
{
    auto now = std::chrono::steady_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

void sleep(unsigned int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// ---- File path functions ----

bool isAbsolutePath(const std::string &name)
{
    return !name.empty() && name[0] == '/';
}

std::string asAbsolutePath(const std::string &name)
{
    if (isAbsolutePath(name)) {
        return name;
    }
    
    // Get current working directory
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd))) {
        return std::string(cwd) + "/" + name;
    }
    return name;  // fallback
}

std::string getAppHome()
{
    // In terminal server mode, use current working directory
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd))) {
        return std::string(cwd);
    }
    return ".";
}

// ---- File request functions ----

int fileReq(int requestor, const std::string &title,
            bool readonly, std::string *fullpath)
{
    // In terminal server mode, file requests are not interactive
    // Return cancel status
    fprintf(stderr, "[WARN] Terminal server: file request '%s' not supported\n", title.c_str());
    return FILEREQ_CANCEL;
}

}  // namespace host

// ---- Debug logging ----

void dbglog(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[DEBUG] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}