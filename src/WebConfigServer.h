#ifndef _INCLUDE_WEB_CONFIG_SERVER_H_
#define _INCLUDE_WEB_CONFIG_SERVER_H_

#include <string>
#include <thread>
#include <atomic>
#include <map>
#include <functional>

/**
 * Lightweight embedded HTTP server for terminal server configuration
 * Provides REST API and web interface for editing wangemu.ini configuration
 */
class WebConfigServer {
public:
    WebConfigServer(int port = 8080, const std::string& iniPath = "wangemu.ini");
    ~WebConfigServer();
    
    /**
     * Start the web server in a background thread
     * @return true if started successfully
     */
    bool start();
    
    /**
     * Stop the web server
     */
    void stop();
    
    /**
     * Check if server is running
     */
    bool isRunning() const { return m_running.load(); }
    
    /**
     * Get server port
     */
    int getPort() const { return m_port; }
    
    /**
     * Set callback for terminal server restart request
     */
    void setRestartCallback(std::function<void()> callback) { m_restartCallback = callback; }

private:
    int m_port;
    std::string m_iniPath;
    std::atomic<bool> m_running{false};
    std::thread m_serverThread;
    std::function<void()> m_restartCallback;
    
    // HTTP request handler
    void serverLoop();
    
    // HTTP request processing
    struct HttpRequest {
        std::string method;
        std::string path;
        std::string query;
        std::map<std::string, std::string> headers;
        std::string body;
    };
    
    struct HttpResponse {
        int status = 200;
        std::map<std::string, std::string> headers;
        std::string body;
    };
    
    // Request handlers
    void handleRequest(int clientSocket, const HttpRequest& request);
    HttpResponse handleGetConfig();
    HttpResponse handlePostConfig(const std::string& body);
    HttpResponse handlePostRestart();
    HttpResponse handlePostReloadConfig();
    HttpResponse handlePostInternalRestart();
    HttpResponse handlePostDiskInsert(const std::string& body);
    HttpResponse handlePostDiskRemove(const std::string& body);
    HttpResponse handleGetRoot();
    HttpResponse serveStaticFile(const std::string& path);
    
    // Utility functions
    HttpRequest parseRequest(const std::string& requestData);
    std::string formatResponse(const HttpResponse& response);
    std::string urlDecode(const std::string& str);
    std::string getContentType(const std::string& path);
    
    // INI file operations
    std::string readIniFile();
    bool writeIniFile(const std::string& content);
    bool validateIniContent(const std::string& content);
};

#endif // _INCLUDE_WEB_CONFIG_SERVER_H_