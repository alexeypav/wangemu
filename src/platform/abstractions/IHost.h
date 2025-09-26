#ifndef _INCLUDE_IHOST_H_
#define _INCLUDE_IHOST_H_

#include <string>
#include <vector>

// Forward declarations
class wxWindow;
class wxRect;

/**
 * Abstract interface for host system services.
 * This separates platform-specific functionality from the core emulation.
 */
class IHost {
public:
    virtual ~IHost() = default;

    // ===== Initialization =====
    
    /**
     * Initialize host services
     * Must be called before any other host functions
     */
    virtual void initialize() = 0;

    // ===== Logging =====
    
    /**
     * Log a debug message
     * @param format Printf-style format string
     * @param ... Format arguments
     */
    virtual void debugLog(const char* format, ...) = 0;

    /**
     * Log an error message
     * @param format Printf-style format string  
     * @param ... Format arguments
     */
    virtual void errorLog(const char* format, ...) = 0;

    // ===== Configuration Management =====
    
    /**
     * Read a string configuration value
     * @param section Configuration section name
     * @param key Configuration key name
     * @param defaultValue Default value if key not found
     * @return Configuration value
     */
    virtual std::string readConfigString(const std::string& section, 
                                        const std::string& key,
                                        const std::string& defaultValue) = 0;

    /**
     * Write a string configuration value
     * @param section Configuration section name
     * @param key Configuration key name
     * @param value Value to write
     */
    virtual void writeConfigString(const std::string& section,
                                  const std::string& key,
                                  const std::string& value) = 0;

    /**
     * Read an integer configuration value
     * @param section Configuration section name
     * @param key Configuration key name
     * @param defaultValue Default value if key not found
     * @return Configuration value
     */
    virtual int readConfigInt(const std::string& section,
                             const std::string& key,
                             int defaultValue) = 0;

    /**
     * Write an integer configuration value
     * @param section Configuration section name
     * @param key Configuration key name
     * @param value Value to write
     */
    virtual void writeConfigInt(const std::string& section,
                               const std::string& key,
                               int value) = 0;

    /**
     * Read a boolean configuration value
     * @param section Configuration section name
     * @param key Configuration key name
     * @param defaultValue Default value if key not found
     * @return Configuration value
     */
    virtual bool readConfigBool(const std::string& section,
                               const std::string& key,
                               bool defaultValue) = 0;

    /**
     * Write a boolean configuration value
     * @param section Configuration section name
     * @param key Configuration key name
     * @param value Value to write
     */
    virtual void writeConfigBool(const std::string& section,
                                const std::string& key,
                                bool value) = 0;

    /**
     * Flush configuration changes to persistent storage
     */
    virtual void flushConfig() = 0;

    // ===== File System =====
    
    /**
     * Get the application's configuration directory
     * @return Path to configuration directory
     */
    virtual std::string getConfigDirectory() = 0;

    /**
     * Get the application's data directory
     * @return Path to data directory
     */
    virtual std::string getDataDirectory() = 0;

    /**
     * Check if a file exists
     * @param filename Path to file
     * @return true if file exists, false otherwise
     */
    virtual bool fileExists(const std::string& filename) = 0;

    /**
     * Get file size in bytes
     * @param filename Path to file
     * @return File size in bytes, -1 on error
     */
    virtual long getFileSize(const std::string& filename) = 0;

    // ===== Time =====
    
    /**
     * Get current time in milliseconds since epoch
     * @return Current time in milliseconds
     */
    virtual long long getCurrentTimeMs() = 0;

    /**
     * Sleep for specified number of milliseconds
     * @param ms Number of milliseconds to sleep
     */
    virtual void sleep(int ms) = 0;

    // ===== GUI Services (optional - may throw if not supported) =====
    
    /**
     * Show a message box (GUI only)
     * @param message Message text
     * @param caption Window caption
     * @param parent Parent window (can be null)
     */
    virtual void showMessageBox(const std::string& message,
                               const std::string& caption,
                               wxWindow* parent = nullptr) = 0;

    /**
     * Show a file dialog (GUI only)
     * @param message Dialog message
     * @param defaultDir Default directory
     * @param defaultFile Default filename
     * @param wildcard File filter wildcard
     * @param parent Parent window (can be null)
     * @return Selected filename, empty string if cancelled
     */
    virtual std::string showFileDialog(const std::string& message,
                                      const std::string& defaultDir,
                                      const std::string& defaultFile,
                                      const std::string& wildcard,
                                      wxWindow* parent = nullptr) = 0;

    /**
     * Check if GUI services are available
     * @return true if GUI services are available, false for headless mode
     */
    virtual bool hasGui() const = 0;
};

/**
 * Factory function to create platform-specific host implementation
 * @return Pointer to host implementation (singleton pattern)
 */
IHost* getHost();

// C-style convenience functions for compatibility
namespace host {
    void initialize();
    void dbglog(const char* format, ...);
    std::string readConfigString(const std::string& section, const std::string& key, const std::string& defaultValue);
    void writeConfigString(const std::string& section, const std::string& key, const std::string& value);
    int readConfigInt(const std::string& section, const std::string& key, int defaultValue);
    void writeConfigInt(const std::string& section, const std::string& key, int value);
    bool readConfigBool(const std::string& section, const std::string& key, bool defaultValue);
    void writeConfigBool(const std::string& section, const std::string& key, bool value);
    void flushConfig();
}

#endif // _INCLUDE_IHOST_H_