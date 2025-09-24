// ============================================================================
// host_headless.cpp - Headless implementation of host services
// 
// This provides minimal implementations of the host:: functions
// for running in headless mode without wxWidgets dependencies.
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

// Simple in-memory configuration storage
static std::map<std::string, std::string> config_store;
static std::string ini_filename = "wangemu.ini";

// Helper to create config key
static std::string makeConfigKey(const std::string &subgroup, const std::string &key) {
    return subgroup + "/" + key;
}

// Simple ini file parser for headless mode
static void loadIniFile(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        fprintf(stderr, "[DEBUG] Could not open %s\n", filename.c_str());
        return; // File doesn't exist or can't be opened
    }
    
    fprintf(stderr, "[DEBUG] Successfully opened %s\n", filename.c_str());
    
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
            
            // Convert wangemu section format to our internal format
            // [wangemu/config-0/io/slot-3] becomes "io/slot-3"
            if (current_section.substr(0, 17) == "wangemu/config-0/" && current_section.length() > 17) {
                std::string subgroup = current_section.substr(17);
                std::string full_key = makeConfigKey(subgroup, key);
                config_store[full_key] = value;
                // fprintf(stderr, "[DEBUG] Parsed: %s = %s\n", full_key.c_str(), value.c_str());
            } else if (current_section.substr(0, 8) == "wangemu/") {
                // Still part of wangemu config but different format - might need it later
                // fprintf(stderr, "[DEBUG] Skipped wangemu section: %s\n", current_section.c_str());
            }
        }
    }
    
    file.close();
}

// Save configuration to INI file
static void saveIniFile(const std::string& filename) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        fprintf(stderr, "[ERROR] Could not write to %s\n", filename.c_str());
        return;
    }
    
    fprintf(stderr, "[INFO] Saving configuration to %s\n", filename.c_str());
    
    // Group keys by section
    std::map<std::string, std::map<std::string, std::string>> sections;
    
    for (const auto& entry : config_store) {
        const std::string& full_key = entry.first;
        const std::string& value = entry.second;
        
        size_t slash_pos = full_key.find('/');
        if (slash_pos != std::string::npos) {
            std::string section = full_key.substr(0, slash_pos);
            std::string key = full_key.substr(slash_pos + 1);
            sections["wangemu/config-0/" + section][key] = value;
        }
    }
    
    // Write INI file header
    file << "[wangemu]\n";
    file << "configversion=1\n";
    
    // Write sections
    for (const auto& section_entry : sections) {
        const std::string& section_name = section_entry.first;
        const std::map<std::string, std::string>& keys = section_entry.second;
        
        file << "[" << section_name << "]\n";
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
    fprintf(stderr, "[INFO] Host subsystem initialized (headless mode)\n");
    
    // Try to load existing wangemu.ini file if it exists
    loadIniFile(ini_filename);
    
    if (!config_store.empty()) {
        fprintf(stderr, "[INFO] Loaded configuration from wangemu.ini\n");
    } else {
        fprintf(stderr, "[INFO] No wangemu.ini found, using defaults\n");
    }
}

void terminate()
{
    if (!config_store.empty()) {
        saveIniFile(ini_filename);
    }
    config_store.clear();
}

// ---- Configuration functions ----

bool configReadStr(const std::string &subgroup,
                   const std::string &key,
                   std::string *val,
                   const std::string *defaultval)
{
    std::string fullkey = makeConfigKey(subgroup, key);
    auto it = config_store.find(fullkey);
    if (it != config_store.end()) {
        *val = it->second;
        return true;
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
    std::string fullkey = makeConfigKey(subgroup, key);
    config_store[fullkey] = val;
}

bool configReadInt(const std::string &subgroup,
                   const std::string &key,
                   int *val,
                   int defaultval)
{
    // Hard-disable GUI-only devices in headless mode
    if (subgroup == "display" &&
        (key == "num_crt" || key == "enable")) {
        *val = 0;
        return true;
    }
    if (subgroup == "printer" && key == "enable") {
        *val = 0;
        return true;
    }
    // Terminal server defaults for headless mode
    if (subgroup == "terminal_server" && key == "num_terms") {
        *val = 1;
        return true;
    }
    if (subgroup == "terminal_server" && key == "mxd_io_addr") {
        // Try to find MXD card in INI configuration first
        for (int slot = 0; slot < 8; slot++) {
            std::string type_key = makeConfigKey("io/slot-" + std::to_string(slot), "type");
            std::string addr_key = makeConfigKey("io/slot-" + std::to_string(slot), "addr");
            
            auto type_it = config_store.find(type_key);
            auto addr_it = config_store.find(addr_key);
            
            if (type_it != config_store.end() && addr_it != config_store.end()) {
                if (type_it->second == "2236 MXD") {
                    // Parse hex address like 0x000
                    const std::string& addr_str = addr_it->second;
                    if (addr_str.length() > 2 && addr_str.substr(0, 2) == "0x") {
                        // Remove "0x" prefix for stoi with base 16
                        std::string hex_part = addr_str.substr(2);
                        *val = std::stoi(hex_part, nullptr, 16);
                        fprintf(stderr, "[INFO] Found MXD in slot %d at address %s (0x%X)\n", 
                                slot, addr_str.c_str(), *val);
                        return true;
                    }
                }
            }
        }
        // Fallback to default if not found in INI
        *val = 0x46;
        return true;
    }

    std::string fullkey = makeConfigKey(subgroup, key);
    auto it = config_store.find(fullkey);
    if (it != config_store.end()) {
        const std::string& value_str = it->second;
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
        return true;
    }
    *val = defaultval;
    return false;
}

void configWriteInt(const std::string &subgroup,
                    const std::string &key,
                    int val)
{
    std::string fullkey = makeConfigKey(subgroup, key);
    config_store[fullkey] = std::to_string(val);
}

void configReadBool(const std::string &subgroup,
                    const std::string &key,
                    bool *val,
                    bool defaultval)
{
    std::string fullkey = makeConfigKey(subgroup, key);
    auto it = config_store.find(fullkey);
    if (it != config_store.end()) {
        *val = (it->second == "true" || it->second == "1");
        return;
    }
    *val = defaultval;
}

void configWriteBool(const std::string &subgroup,
                     const std::string &key,
                     bool val)
{
    std::string fullkey = makeConfigKey(subgroup, key);
    config_store[fullkey] = val ? "true" : "false";
}

// Forward declarations for headless build
class wxWindow;
class wxRect;

// Window geometry functions - no-ops for headless
void configReadWinGeom(wxWindow *wxwin,
                       const std::string &subgroup,
                       wxRect *default_geom,
                       bool client_size)
{
    // no-op in headless mode
}

void configWriteWinGeom(wxWindow *wxwin,
                        const std::string &subgroup,
                        bool client_size)
{
    // no-op in headless mode
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
    // In headless mode, use current working directory
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
    // In headless mode, file requests are not interactive
    // Return cancel status
    fprintf(stderr, "[WARN] Headless: file request '%s' not supported\n", title.c_str());
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