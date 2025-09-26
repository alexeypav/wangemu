#ifndef _INCLUDE_IPLATFORM_H_
#define _INCLUDE_IPLATFORM_H_

/**
 * Platform identification and capability detection
 */
namespace Platform {

    /**
     * Platform types
     */
    enum Type {
        Windows,
        Linux,
        MacOS,
        Unknown
    };

    /**
     * Build configuration types
     */
    enum BuildConfig {
        GUI,        // Full GUI version with wxWidgets
        Headless,   // Terminal server without GUI
        Unknown_Config
    };

    /**
     * Get the current platform type
     * @return Platform type
     */
    Type getPlatformType();

    /**
     * Get the current build configuration
     * @return Build configuration type
     */
    BuildConfig getBuildConfig();

    /**
     * Get platform name as string
     * @return Platform name
     */
    const char* getPlatformName();

    /**
     * Get build configuration name as string
     * @return Build configuration name
     */
    const char* getBuildConfigName();

    /**
     * Check if current platform is Windows
     * @return true if Windows, false otherwise
     */
    inline bool isWindows() { return getPlatformType() == Windows; }

    /**
     * Check if current platform is POSIX-compliant (Linux/MacOS)
     * @return true if POSIX, false otherwise
     */
    inline bool isPosix() { 
        Type t = getPlatformType();
        return t == Linux || t == MacOS; 
    }

    /**
     * Check if current build has GUI support
     * @return true if GUI available, false for headless
     */
    inline bool hasGui() { return getBuildConfig() == GUI; }

    /**
     * Check if current build is headless
     * @return true if headless, false otherwise
     */
    inline bool isHeadless() { return getBuildConfig() == Headless; }

    // Platform-specific constants
    namespace Constants {
        // Path separators
        #ifdef _WIN32
            const char PATH_SEPARATOR = '\\';
            const char PATH_SEPARATOR_OTHER = '/';
            const char* const PATH_SEPARATOR_STR = "\\";
        #else
            const char PATH_SEPARATOR = '/';
            const char PATH_SEPARATOR_OTHER = '\\';
            const char* const PATH_SEPARATOR_STR = "/";
        #endif

        // Line endings
        #ifdef _WIN32
            const char* const LINE_ENDING = "\r\n";
        #else
            const char* const LINE_ENDING = "\n";
        #endif

        // Dynamic library extensions
        #ifdef _WIN32
            const char* const DLL_EXTENSION = ".dll";
        #elif defined(__APPLE__)
            const char* const DLL_EXTENSION = ".dylib";
        #else
            const char* const DLL_EXTENSION = ".so";
        #endif
    }

    // Utility functions
    namespace Utils {
        /**
         * Convert path separators to platform-specific format
         * @param path Path string to normalize
         * @return Normalized path
         */
        std::string normalizePath(const std::string& path);

        /**
         * Join path components with platform-specific separator
         * @param components Path components to join
         * @return Joined path
         */
        std::string joinPath(const std::vector<std::string>& components);

        /**
         * Get directory part of path
         * @param path Full path
         * @return Directory part
         */
        std::string getDirectory(const std::string& path);

        /**
         * Get filename part of path
         * @param path Full path
         * @return Filename part
         */
        std::string getFilename(const std::string& path);
    }
}

#endif // _INCLUDE_IPLATFORM_H_