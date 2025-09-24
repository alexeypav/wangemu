#include "WebConfigServer.h"
#include "host.h"
#include "system2200.h"
#include "SysCfgState.h"
#include "IoCardDisk.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <signal.h>
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstring>
#include <regex>
#include <thread>
#include <chrono>

WebConfigServer::WebConfigServer(int port, const std::string& iniPath) 
    : m_port(port), m_iniPath(iniPath) 
{
}

WebConfigServer::~WebConfigServer() {
    stop();
}

bool WebConfigServer::start() {
    if (m_running.load()) {
        return true; // Already running
    }
    
    m_running.store(true);
    m_serverThread = std::thread(&WebConfigServer::serverLoop, this);
    
    // Give it a moment to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    return m_running.load();
}

void WebConfigServer::stop() {
    m_running.store(false);
    
    if (m_serverThread.joinable()) {
        m_serverThread.join();
    }
}

void WebConfigServer::serverLoop() {
    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket < 0) {
        std::cerr << "[ERROR] Failed to create socket\n";
        m_running.store(false);
        return;
    }
    
    // Enable address reuse
    int opt = 1;
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(m_port);
    
    if (bind(serverSocket, (struct sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "[ERROR] Failed to bind to port " << m_port << "\n";
        close(serverSocket);
        m_running.store(false);
        return;
    }
    
    if (listen(serverSocket, 5) < 0) {
        std::cerr << "[ERROR] Failed to listen on socket\n";
        close(serverSocket);
        m_running.store(false);
        return;
    }
    
    std::cout << "[INFO] Web configuration server started on port " << m_port << "\n";
    std::cout << "[INFO] Open http://localhost:" << m_port << " in your browser\n";
    
    while (m_running.load()) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(serverSocket, &readfds);
        
        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        
        int activity = select(serverSocket + 1, &readfds, NULL, NULL, &timeout);
        
        if (!m_running.load()) break;
        
        if (activity > 0 && FD_ISSET(serverSocket, &readfds)) {
            struct sockaddr_in clientAddr;
            socklen_t addrLen = sizeof(clientAddr);
            
            int clientSocket = accept(serverSocket, (struct sockaddr*)&clientAddr, &addrLen);
            if (clientSocket >= 0) {
                // Read request
                char buffer[4096] = {0};
                ssize_t bytesRead = read(clientSocket, buffer, sizeof(buffer) - 1);
                
                if (bytesRead > 0) {
                    std::string requestData(buffer, bytesRead);
                    HttpRequest request = parseRequest(requestData);
                    handleRequest(clientSocket, request);
                }
                
                close(clientSocket);
            }
        }
    }
    
    close(serverSocket);
    std::cout << "[INFO] Web configuration server stopped\n";
}

void WebConfigServer::handleRequest(int clientSocket, const HttpRequest& request) {
    HttpResponse response;
    
    if (request.method == "GET") {
        if (request.path == "/" || request.path == "/index.html") {
            response = handleGetRoot();
        } else if (request.path == "/api/config") {
            response = handleGetConfig();
        } else if (request.path.find("/static/") == 0) {
            response = serveStaticFile(request.path);
        } else {
            response.status = 404;
            response.body = "Not Found";
        }
    } else if (request.method == "POST") {
        if (request.path == "/api/config") {
            response = handlePostConfig(request.body);
        } else if (request.path == "/api/restart") {
            response = handlePostRestart();
        } else if (request.path == "/api/reload") {
            response = handlePostReloadConfig();
        } else if (request.path == "/api/internal-restart") {
            response = handlePostInternalRestart();
        } else if (request.path == "/api/disk-insert") {
            response = handlePostDiskInsert(request.body);
        } else if (request.path == "/api/disk-remove") {
            response = handlePostDiskRemove(request.body);
        } else {
            response.status = 404;
            response.body = "Not Found";
        }
    } else {
        response.status = 405;
        response.body = "Method Not Allowed";
    }
    
    std::string responseStr = formatResponse(response);
    write(clientSocket, responseStr.c_str(), responseStr.length());
}

WebConfigServer::HttpResponse WebConfigServer::handleGetConfig() {
    HttpResponse response;
    response.headers["Content-Type"] = "application/json";
    response.headers["Access-Control-Allow-Origin"] = "*";
    
    std::string iniContent = readIniFile();
    if (iniContent.empty()) {
        response.status = 500;
        response.body = "{\"error\":\"Failed to read configuration file\"}";
        return response;
    }
    
    // Convert INI content to JSON for easier web editing
    std::ostringstream json;
    json << "{\"iniContent\":\"";
    
    // Escape the INI content for JSON
    for (char c : iniContent) {
        if (c == '"') json << "\\\"";
        else if (c == '\\') json << "\\\\";
        else if (c == '\n') json << "\\n";
        else if (c == '\r') json << "\\r";
        else if (c == '\t') json << "\\t";
        else json << c;
    }
    
    json << "\"}";
    response.body = json.str();
    return response;
}

WebConfigServer::HttpResponse WebConfigServer::handlePostConfig(const std::string& body) {
    HttpResponse response;
    response.headers["Content-Type"] = "application/json";
    response.headers["Access-Control-Allow-Origin"] = "*";
    
    // Parse JSON body to extract INI content - simple string search approach
    // Look for "iniContent":" and extract content until closing "
    size_t startPos = body.find("\"iniContent\":\"");
    if (startPos == std::string::npos) {
        response.status = 400;
        response.body = "{\"error\":\"Invalid JSON format - missing iniContent field\"}";
        return response;
    }
    
    startPos += 14; // Skip past "iniContent":"
    
    // Find the closing quote, handling escaped quotes
    std::string iniContent;
    bool escaped = false;
    for (size_t i = startPos; i < body.length(); ++i) {
        char c = body[i];
        if (escaped) {
            switch (c) {
                case '"': iniContent += '"'; break;
                case '\\': iniContent += '\\'; break;
                case 'n': iniContent += '\n'; break;
                case 'r': iniContent += '\r'; break;
                case 't': iniContent += '\t'; break;
                default: iniContent += c; break;
            }
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            break; // Found closing quote
        } else {
            iniContent += c;
        }
    }
    
    // No need to unescape since we already handled it above
    std::string& unescaped = iniContent;
    
    // Validate INI content
    if (!validateIniContent(unescaped)) {
        response.status = 400;
        response.body = "{\"error\":\"Invalid INI configuration\"}";
        return response;
    }
    
    // Write to file
    if (!writeIniFile(unescaped)) {
        response.status = 500;
        response.body = "{\"error\":\"Failed to write configuration file\"}";
        return response;
    }
    
    response.body = "{\"status\":\"success\"}";
    return response;
}

WebConfigServer::HttpResponse WebConfigServer::handlePostInternalRestart() {
    HttpResponse response;
    response.headers["Content-Type"] = "application/json";
    response.headers["Access-Control-Allow-Origin"] = "*";
    
    try {
        // This implements the GUI "OK, reboot" functionality:
        // 1. Reload host configuration from INI file
        // 2. Create new SysCfgState from host config
        // 3. Apply configuration using system2200::setConfig (internal restart)
        
        std::cout << "[INFO] Requesting safe internal system restart...\n";
        
        // Instead of doing the restart directly (which can cause race conditions),
        // set a flag for the main thread to perform the restart safely
        extern void requestInternalRestart();  // Defined in main_headless.cpp
        requestInternalRestart();
        
        // Give the main thread a moment to start processing
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        response.body = "{\"status\":\"internal restart requested - system will reconfigure safely\"}";
        
    } catch (const std::exception& e) {
        response.status = 500;
        response.body = "{\"error\":\"Failed to perform internal restart: " + std::string(e.what()) + "\"}";
        std::cerr << "[ERROR] Failed to perform internal restart: " << e.what() << "\n";
    } catch (...) {
        response.status = 500;
        response.body = "{\"error\":\"Failed to perform internal restart: unknown error\"}";
        std::cerr << "[ERROR] Failed to perform internal restart: unknown error\n";
    }
    
    return response;
}

WebConfigServer::HttpResponse WebConfigServer::handlePostReloadConfig() {
    HttpResponse response;
    response.headers["Content-Type"] = "application/json";
    response.headers["Access-Control-Allow-Origin"] = "*";
    
    try {
        // Reload configuration from the INI file into the host system only
        // This is for configuration that doesn't require restart
        host::loadConfigFile(m_iniPath);
        response.body = "{\"status\":\"configuration reloaded successfully\"}";
        std::cout << "[INFO] Configuration reloaded from " << m_iniPath << " via web interface\n";
    } catch (const std::exception& e) {
        response.status = 500;
        response.body = "{\"error\":\"Failed to reload configuration: " + std::string(e.what()) + "\"}";
        std::cerr << "[ERROR] Failed to reload configuration: " << e.what() << "\n";
    } catch (...) {
        response.status = 500;
        response.body = "{\"error\":\"Failed to reload configuration: unknown error\"}";
        std::cerr << "[ERROR] Failed to reload configuration: unknown error\n";
    }
    
    return response;
}

WebConfigServer::HttpResponse WebConfigServer::handlePostRestart() {
    HttpResponse response;
    response.headers["Content-Type"] = "application/json";
    response.headers["Access-Control-Allow-Origin"] = "*";
    
    if (m_restartCallback) {
        // First reload the configuration, then schedule restart after sending response
        std::thread([this]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            
            try {
                // Reload configuration before restart to ensure changes are applied
                std::cout << "[INFO] Reloading configuration before restart...\n";
                host::loadConfigFile(m_iniPath);
                std::cout << "[INFO] Configuration reloaded successfully\n";
            } catch (const std::exception& e) {
                std::cerr << "[WARN] Failed to reload configuration before restart: " << e.what() << "\n";
            } catch (...) {
                std::cerr << "[WARN] Failed to reload configuration before restart: unknown error\n";
            }
            
            // Now restart
            m_restartCallback();
        }).detach();
        
        response.body = "{\"status\":\"restarting with updated configuration\"}";
    } else {
        response.status = 501;
        response.body = "{\"error\":\"Restart not implemented\"}";
    }
    
    return response;
}

WebConfigServer::HttpResponse WebConfigServer::handlePostDiskInsert(const std::string& body) {
    HttpResponse response;
    response.headers["Content-Type"] = "application/json";
    response.headers["Access-Control-Allow-Origin"] = "*";
    
    try {
        // Parse JSON request: {"slot": 0, "drive": 0, "filename": "/path/to/disk.wvd"}
        std::cout << "[INFO] Disk insert request: " << body << "\n";
        
        // Simple JSON parsing for slot, drive, and filename
        int slot = -1, drive = -1;
        std::string filename;
        
        size_t slotPos = body.find("\"slot\":");
        if (slotPos != std::string::npos) {
            slotPos += 7; // Skip "slot":
            while (slotPos < body.size() && (body[slotPos] == ' ' || body[slotPos] == '\t')) slotPos++;
            if (slotPos < body.size() && isdigit(body[slotPos])) {
                slot = body[slotPos] - '0';
            }
        }
        
        size_t drivePos = body.find("\"drive\":");
        if (drivePos != std::string::npos) {
            drivePos += 8; // Skip "drive":
            while (drivePos < body.size() && (body[drivePos] == ' ' || body[drivePos] == '\t')) drivePos++;
            if (drivePos < body.size() && isdigit(body[drivePos])) {
                drive = body[drivePos] - '0';
            }
        }
        
        size_t filenamePos = body.find("\"filename\":");
        if (filenamePos != std::string::npos) {
            filenamePos += 11; // Skip "filename":
            while (filenamePos < body.size() && (body[filenamePos] == ' ' || body[filenamePos] == '\t' || body[filenamePos] == '"')) filenamePos++;
            size_t endPos = body.find('"', filenamePos);
            if (endPos != std::string::npos) {
                filename = body.substr(filenamePos, endPos - filenamePos);
            }
        }
        
        if (slot < 0 || drive < 0 || filename.empty()) {
            response.status = 400;
            response.body = "{\"error\":\"Invalid request format. Expected {slot: N, drive: N, filename: 'path'}\"}";
            return response;
        }
        
        std::cout << "[INFO] Inserting disk: slot=" << slot << ", drive=" << drive << ", file=" << filename << "\n";
        
        // Use the same direct disk operation as the GUI
        bool ok = IoCardDisk::wvdInsertDisk(slot, drive, filename);
        
        if (ok) {
            response.body = "{\"status\":\"disk inserted successfully\"}";
            std::cout << "[INFO] Disk inserted successfully\n";
        } else {
            response.status = 500;
            response.body = "{\"error\":\"Failed to insert disk\"}";
            std::cout << "[ERROR] Failed to insert disk\n";
        }
        
    } catch (const std::exception& e) {
        response.status = 500;
        response.body = "{\"error\":\"Failed to insert disk: " + std::string(e.what()) + "\"}";
        std::cerr << "[ERROR] Failed to insert disk: " << e.what() << "\n";
    } catch (...) {
        response.status = 500;
        response.body = "{\"error\":\"Failed to insert disk: unknown error\"}";
        std::cerr << "[ERROR] Failed to insert disk: unknown error\n";
    }
    
    return response;
}

WebConfigServer::HttpResponse WebConfigServer::handlePostDiskRemove(const std::string& body) {
    HttpResponse response;
    response.headers["Content-Type"] = "application/json";
    response.headers["Access-Control-Allow-Origin"] = "*";
    
    try {
        // Parse JSON request: {"slot": 0, "drive": 0}
        std::cout << "[INFO] Disk remove request: " << body << "\n";
        
        // Simple JSON parsing for slot and drive
        int slot = -1, drive = -1;
        
        size_t slotPos = body.find("\"slot\":");
        if (slotPos != std::string::npos) {
            slotPos += 7; // Skip "slot":
            while (slotPos < body.size() && (body[slotPos] == ' ' || body[slotPos] == '\t')) slotPos++;
            if (slotPos < body.size() && isdigit(body[slotPos])) {
                slot = body[slotPos] - '0';
            }
        }
        
        size_t drivePos = body.find("\"drive\":");
        if (drivePos != std::string::npos) {
            drivePos += 8; // Skip "drive":
            while (drivePos < body.size() && (body[drivePos] == ' ' || body[drivePos] == '\t')) drivePos++;
            if (drivePos < body.size() && isdigit(body[drivePos])) {
                drive = body[drivePos] - '0';
            }
        }
        
        if (slot < 0 || drive < 0) {
            response.status = 400;
            response.body = "{\"error\":\"Invalid request format. Expected {slot: N, drive: N}\"}";
            return response;
        }
        
        std::cout << "[INFO] Removing disk: slot=" << slot << ", drive=" << drive << "\n";
        
        // Use the same direct disk operation as the GUI
        bool ok = IoCardDisk::wvdRemoveDisk(slot, drive);
        
        if (ok) {
            response.body = "{\"status\":\"disk removed successfully\"}";
            std::cout << "[INFO] Disk removed successfully\n";
        } else {
            response.status = 500;
            response.body = "{\"error\":\"Failed to remove disk\"}";
            std::cout << "[ERROR] Failed to remove disk\n";
        }
        
    } catch (const std::exception& e) {
        response.status = 500;
        response.body = "{\"error\":\"Failed to remove disk: " + std::string(e.what()) + "\"}";
        std::cerr << "[ERROR] Failed to remove disk: " << e.what() << "\n";
    } catch (...) {
        response.status = 500;
        response.body = "{\"error\":\"Failed to remove disk: unknown error\"}";
        std::cerr << "[ERROR] Failed to remove disk: unknown error\n";
    }
    
    return response;
}

WebConfigServer::HttpResponse WebConfigServer::handleGetRoot() {
    HttpResponse response;
    response.headers["Content-Type"] = "text/html";
    
    // Build user-friendly GUI-style configuration interface
    std::ostringstream html;
    html << "<!DOCTYPE html>\n";
    html << "<html lang=\"en\">\n";
    html << "<head>\n";
    html << "    <meta charset=\"UTF-8\">\n";
    html << "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n";
    html << "    <title>Wang 2200 Terminal Server Configuration</title>\n";
    html << "    <style>\n";
    html << "        body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; margin: 0; padding: 20px; background: #f0f0f0; }\n";
    html << "        .container { max-width: 900px; margin: 0 auto; }\n";
    html << "        .config-panel { background: #fff; border: 1px solid #ccc; border-radius: 6px; margin-bottom: 20px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }\n";
    html << "        .panel-header { background: linear-gradient(to bottom, #f8f8f8, #e8e8e8); border-bottom: 1px solid #ccc; padding: 12px 20px; font-weight: bold; color: #333; border-radius: 6px 6px 0 0; }\n";
    html << "        .panel-body { padding: 20px; }\n";
    html << "        .form-group { margin-bottom: 15px; }\n";
    html << "        .form-group label { display: block; margin-bottom: 5px; font-weight: bold; color: #333; }\n";
    html << "        .form-group select, .form-group input[type=\"text\"], .form-group input[type=\"number\"] { padding: 6px 8px; border: 1px solid #ccc; border-radius: 3px; font-size: 12px; background: white; }\n";
    html << "        .form-group select { width: 150px; }\n";
    html << "        .form-group input[type=\"text\"] { width: 200px; }\n";
    html << "        .form-group input[type=\"number\"] { width: 80px; }\n";
    html << "        .terminal-grid { display: grid; grid-template-columns: 80px 100px 200px 80px 120px auto; gap: 10px; align-items: center; margin-bottom: 8px; }\n";
    html << "        .terminal-grid:first-child { font-weight: bold; background: #f5f5f5; padding: 8px 0; margin-bottom: 15px; }\n";
    html << "        .terminal-grid input[type=\"checkbox\"] { justify-self: center; }\n";
    html << "        .num-terminals { margin-bottom: 20px; }\n";
    html << "        .num-terminals input[type=\"radio\"] { margin-right: 5px; margin-left: 15px; }\n";
    html << "        .buttons { text-align: center; margin: 20px 0; }\n";
    html << "        .btn { background: #0078d4; color: white; border: none; padding: 8px 16px; margin: 0 5px; border-radius: 3px; cursor: pointer; font-size: 12px; }\n";
    html << "        .btn:hover { background: #106ebe; }\n";
    html << "        .btn.secondary { background: #6c757d; }\n";
    html << "        .btn.secondary:hover { background: #5a6268; }\n";
    html << "        .btn.danger { background: #dc3545; }\n";
    html << "        .btn.danger:hover { background: #c82333; }\n";
    html << "        .status { margin: 15px 0; padding: 10px; border-radius: 4px; text-align: center; }\n";
    html << "        .status.success { background: #d4edda; color: #155724; border: 1px solid #c3e6cb; }\n";
    html << "        .status.error { background: #f8d7da; color: #721c24; border: 1px solid #f5c6cb; }\n";
    html << "        .row { display: flex; gap: 20px; align-items: center; margin-bottom: 15px; }\n";
    html << "        .checkbox-group { display: flex; align-items: center; gap: 8px; }\n";
    html << "        h1 { text-align: center; color: #333; margin-bottom: 30px; }\n";
    html << "        .advanced-toggle { margin-top: 20px; text-align: center; }\n";
    html << "        .advanced-config { display: none; margin-top: 20px; }\n";
    html << "        .advanced-config textarea { width: 100%; height: 200px; font-family: monospace; font-size: 11px; }\n";
    html << "    </style>\n";
    html << "</head>\n";
    html << "<body>\n";
    html << "    <div class=\"container\">\n";
    html << "        <h1>Wang 2200 Terminal Server Configuration</h1>\n";
    html << "        \n";
    html << "        <!-- System Configuration Panel -->\n";
    html << "        <div class=\"config-panel\">\n";
    html << "            <div class=\"panel-header\">System Configuration</div>\n";
    html << "            <div class=\"panel-body\">\n";
    html << "                <div class=\"row\">\n";
    html << "                    <div class=\"form-group\">\n";
    html << "                        <label for=\"cpu\">CPU:</label>\n";
    html << "                        <select id=\"cpu\">\n";
    html << "                            <option value=\"2200B\">2200B</option>\n";
    html << "                            <option value=\"2200T\">2200T</option>\n";
    html << "                            <option value=\"2200VP\">2200VP</option>\n";
    html << "                            <option value=\"2200MVP-C\">2200MVP-C</option>\n";
    html << "                            <option value=\"MicroVP\">MicroVP</option>\n";
    html << "                        </select>\n";
    html << "                    </div>\n";
    html << "                    <div class=\"form-group\">\n";
    html << "                        <label for=\"ram\">RAM:</label>\n";
    html << "                        <select id=\"ram\">\n";
    html << "                            <option value=\"512\">512 KB</option>\n";
    html << "                            <option value=\"256\">256 KB</option>\n";
    html << "                            <option value=\"128\">128 KB</option>\n";
    html << "                            <option value=\"64\">64 KB</option>\n";
    html << "                        </select>\n";
    html << "                    </div>\n";
    html << "                </div>\n";
    html << "                <div class=\"checkbox-group\">\n";
    html << "                    <input type=\"checkbox\" id=\"warnInvalidIo\"> <label for=\"warnInvalidIo\">Warn on Invalid I/O Device Access</label>\n";
    html << "                </div>\n";
    html << "            </div>\n";
    html << "        </div>\n";
    html << "        \n";
    html << "        <!-- Terminal Configuration Panel -->\n";
    html << "        <div class=\"config-panel\">\n";
    html << "            <div class=\"panel-header\">Terminal Multiplexer Configuration</div>\n";
    html << "            <div class=\"panel-body\">\n";
    html << "                <div class=\"num-terminals\">\n";
    html << "                    <label>Number of terminals:</label>\n";
    html << "                    <input type=\"radio\" name=\"numTerminals\" value=\"1\" id=\"term1\" checked> <label for=\"term1\">1</label>\n";
    html << "                    <input type=\"radio\" name=\"numTerminals\" value=\"2\" id=\"term2\"> <label for=\"term2\">2</label>\n";
    html << "                    <input type=\"radio\" name=\"numTerminals\" value=\"3\" id=\"term3\"> <label for=\"term3\">3</label>\n";
    html << "                    <input type=\"radio\" name=\"numTerminals\" value=\"4\" id=\"term4\"> <label for=\"term4\">4</label>\n";
    html << "                </div>\n";
    html << "                \n";
    html << "                <div class=\"terminal-grid\">\n";
    html << "                    <div>Terminal</div>\n";
    html << "                    <div>Use Port</div>\n";
    html << "                    <div>Port Name</div>\n";
    html << "                    <div>Baud Rate</div>\n";
    html << "                    <div>XON/XOFF Flow</div>\n";
    html << "                    <div></div>\n";
    html << "                </div>\n";
    html << "                \n";
    html << "                <div class=\"terminal-grid\">\n";
    html << "                    <div>Terminal 1</div>\n";
    html << "                    <input type=\"checkbox\" id=\"term1_enabled\" checked>\n";
    html << "                    <input type=\"text\" id=\"term1_port\" value=\"/dev/ttyUSB0\" placeholder=\"/dev/ttyUSB0\">\n";
    html << "                    <select id=\"term1_baud\">\n";
    html << "                        <option value=\"19200\" selected>19200</option>\n";
    html << "                        <option value=\"9600\">9600</option>\n";
    html << "                        <option value=\"4800\">4800</option>\n";
    html << "                        <option value=\"2400\">2400</option>\n";
    html << "                        <option value=\"1200\">1200</option>\n";
    html << "                    </select>\n";
    html << "                    <input type=\"checkbox\" id=\"term1_flow\" checked>\n";
    html << "                    <div></div>\n";
    html << "                </div>\n";
    html << "                \n";
    html << "                <div class=\"terminal-grid\">\n";
    html << "                    <div>Terminal 2</div>\n";
    html << "                    <input type=\"checkbox\" id=\"term2_enabled\">\n";
    html << "                    <input type=\"text\" id=\"term2_port\" value=\"/dev/ttyUSB1\" placeholder=\"/dev/ttyUSB1\">\n";
    html << "                    <select id=\"term2_baud\">\n";
    html << "                        <option value=\"19200\" selected>19200</option>\n";
    html << "                        <option value=\"9600\">9600</option>\n";
    html << "                        <option value=\"4800\">4800</option>\n";
    html << "                        <option value=\"2400\">2400</option>\n";
    html << "                        <option value=\"1200\">1200</option>\n";
    html << "                    </select>\n";
    html << "                    <input type=\"checkbox\" id=\"term2_flow\" checked>\n";
    html << "                    <div></div>\n";
    html << "                </div>\n";
    html << "                \n";
    html << "                <div class=\"terminal-grid\">\n";
    html << "                    <div>Terminal 3</div>\n";
    html << "                    <input type=\"checkbox\" id=\"term3_enabled\">\n";
    html << "                    <input type=\"text\" id=\"term3_port\" value=\"/dev/ttyUSB2\" placeholder=\"/dev/ttyUSB2\">\n";
    html << "                    <select id=\"term3_baud\">\n";
    html << "                        <option value=\"19200\" selected>19200</option>\n";
    html << "                        <option value=\"9600\">9600</option>\n";
    html << "                        <option value=\"4800\">4800</option>\n";
    html << "                        <option value=\"2400\">2400</option>\n";
    html << "                        <option value=\"1200\">1200</option>\n";
    html << "                    </select>\n";
    html << "                    <input type=\"checkbox\" id=\"term3_flow\" checked>\n";
    html << "                    <div></div>\n";
    html << "                </div>\n";
    html << "                \n";
    html << "                <div class=\"terminal-grid\">\n";
    html << "                    <div>Terminal 4</div>\n";
    html << "                    <input type=\"checkbox\" id=\"term4_enabled\">\n";
    html << "                    <input type=\"text\" id=\"term4_port\" value=\"/dev/ttyUSB3\" placeholder=\"/dev/ttyUSB3\">\n";
    html << "                    <select id=\"term4_baud\">\n";
    html << "                        <option value=\"19200\" selected>19200</option>\n";
    html << "                        <option value=\"9600\">9600</option>\n";
    html << "                        <option value=\"4800\">4800</option>\n";
    html << "                        <option value=\"2400\">2400</option>\n";
    html << "                        <option value=\"1200\">1200</option>\n";
    html << "                    </select>\n";
    html << "                    <input type=\"checkbox\" id=\"term4_flow\" checked>\n";
    html << "                    <div></div>\n";
    html << "                </div>\n";
    html << "            </div>\n";
    html << "        </div>\n";
    html << "        \n";
    html << "        <!-- Disk Controller Configuration Panel -->\n";
    html << "        <div class=\"config-panel\">\n";
    html << "            <div class=\"panel-header\">Disk Controller Configuration</div>\n";
    html << "            <div class=\"panel-body\">\n";
    html << "                <div class=\"row\">\n";
    html << "                    <div class=\"checkbox-group\">\n";
    html << "                        <input type=\"checkbox\" id=\"diskEnabled\" checked> <label for=\"diskEnabled\">Enable Disk Controller</label>\n";
    html << "                    </div>\n";
    html << "                </div>\n";
    html << "                \n";
    html << "                <div class=\"row\">\n";
    html << "                    <div class=\"form-group\">\n";
    html << "                        <label for=\"diskType\">Controller Type:</label>\n";
    html << "                        <select id=\"diskType\">\n";
    html << "                            <option value=\"6541\">6541 Disk Controller</option>\n";
    html << "                            <option value=\"6471\">6471 Disk Controller</option>\n";
    html << "                        </select>\n";
    html << "                    </div>\n";
    html << "                    <div class=\"form-group\">\n";
    html << "                        <label for=\"diskAddr\">I/O Address:</label>\n";
    html << "                        <select id=\"diskAddr\">\n";
    html << "                            <option value=\"0x310\">0x310</option>\n";
    html << "                            <option value=\"0x320\">0x320</option>\n";
    html << "                            <option value=\"0x330\">0x330</option>\n";
    html << "                            <option value=\"0x340\">0x340</option>\n";
    html << "                        </select>\n";
    html << "                    </div>\n";
    html << "                </div>\n";
    html << "                \n";
    html << "                <div class=\"row\">\n";
    html << "                    <div class=\"form-group\">\n";
    html << "                        <label for=\"numDrives\">Number of drives:</label>\n";
    html << "                        <input type=\"radio\" name=\"numDrives\" value=\"1\" id=\"drive1\"> <label for=\"drive1\">1</label>\n";
    html << "                        <input type=\"radio\" name=\"numDrives\" value=\"2\" id=\"drive2\" checked> <label for=\"drive2\">2</label>\n";
    html << "                        <input type=\"radio\" name=\"numDrives\" value=\"3\" id=\"drive3\"> <label for=\"drive3\">3</label>\n";
    html << "                        <input type=\"radio\" name=\"numDrives\" value=\"4\" id=\"drive4\"> <label for=\"drive4\">4</label>\n";
    html << "                    </div>\n";
    html << "                </div>\n";
    html << "                \n";
    html << "                <div class=\"row\">\n";
    html << "                    <div class=\"form-group\">\n";
    html << "                        <label>Controller Intelligence:</label>\n";
    html << "                        <input type=\"radio\" name=\"intelligence\" value=\"dumb\" id=\"dumb\"> <label for=\"dumb\">Dumb</label>\n";
    html << "                        <input type=\"radio\" name=\"intelligence\" value=\"smart\" id=\"smart\" checked> <label for=\"smart\">Intelligent</label>\n";
    html << "                    </div>\n";
    html << "                </div>\n";
    html << "                \n";
    html << "                <div class=\"checkbox-group\">\n";
    html << "                    <input type=\"checkbox\" id=\"warnMismatch\" checked> <label for=\"warnMismatch\">Warn when the media doesn't match the controller intelligence</label>\n";
    html << "                </div>\n";
    html << "                \n";
    html << "                <h4 style=\"margin-top: 20px; margin-bottom: 10px;\">Disk Files</h4>\n";
    html << "                <div class=\"form-group\">\n";
    html << "                    <label>Drive 0 (Slot 1, Drive 0):</label>\n";
    html << "                    <div style=\"display: flex; align-items: center; gap: 10px;\">\n";
    html << "                        <input type=\"text\" id=\"disk0File\" style=\"width: 300px;\" placeholder=\"Path to disk image file (.wvd)\">\n";
    html << "                        <button type=\"button\" class=\"btn secondary\" onclick=\"insertDisk(1, 0)\">Insert</button>\n";
    html << "                        <button type=\"button\" class=\"btn danger\" onclick=\"removeDisk(1, 0)\">Remove</button>\n";
    html << "                        <span id=\"disk0Status\" style=\"color: #666;\"></span>\n";
    html << "                    </div>\n";
    html << "                </div>\n";
    html << "                \n";
    html << "                <div class=\"form-group\">\n";
    html << "                    <label>Drive 1 (Slot 1, Drive 1):</label>\n";
    html << "                    <div style=\"display: flex; align-items: center; gap: 10px;\">\n";
    html << "                        <input type=\"text\" id=\"disk1File\" style=\"width: 300px;\" placeholder=\"Path to disk image file (.wvd)\">\n";
    html << "                        <button type=\"button\" class=\"btn secondary\" onclick=\"insertDisk(1, 1)\">Insert</button>\n";
    html << "                        <button type=\"button\" class=\"btn danger\" onclick=\"removeDisk(1, 1)\">Remove</button>\n";
    html << "                        <span id=\"disk1Status\" style=\"color: #666;\"></span>\n";
    html << "                    </div>\n";
    html << "                </div>\n";
    html << "                \n";
    html << "                <div class=\"form-group\">\n";
    html << "                    <label>Drive 2 (Slot 1, Drive 2):</label>\n";
    html << "                    <div style=\"display: flex; align-items: center; gap: 10px;\">\n";
    html << "                        <input type=\"text\" id=\"disk2File\" style=\"width: 300px;\" placeholder=\"Path to disk image file (.wvd)\">\n";
    html << "                        <button type=\"button\" class=\"btn secondary\" onclick=\"insertDisk(1, 2)\">Insert</button>\n";
    html << "                        <button type=\"button\" class=\"btn danger\" onclick=\"removeDisk(1, 2)\">Remove</button>\n";
    html << "                        <span id=\"disk2Status\" style=\"color: #666;\"></span>\n";
    html << "                    </div>\n";
    html << "                </div>\n";
    html << "                \n";
    html << "                <div class=\"form-group\">\n";
    html << "                    <label>Drive 3 (Slot 1, Drive 3):</label>\n";
    html << "                    <div style=\"display: flex; align-items: center; gap: 10px;\">\n";
    html << "                        <input type=\"text\" id=\"disk3File\" style=\"width: 300px;\" placeholder=\"Path to disk image file (.wvd)\">\n";
    html << "                        <button type=\"button\" class=\"btn secondary\" onclick=\"insertDisk(1, 3)\">Insert</button>\n";
    html << "                        <button type=\"button\" class=\"btn danger\" onclick=\"removeDisk(1, 3)\">Remove</button>\n";
    html << "                        <span id=\"disk3Status\" style=\"color: #666;\"></span>\n";
    html << "                    </div>\n";
    html << "                </div>\n";
    html << "            </div>\n";
    html << "        </div>\n";
    html << "        \n";
    html << "        <div class=\"buttons\">\n";
    html << "            <button class=\"btn\" onclick=\"saveAndApplyConfig()\">OK, Apply &amp; Restart</button>\n";
    html << "            <button class=\"btn secondary\" onclick=\"saveConfig()\">Save Only</button>\n";
    html << "            <button class=\"btn secondary\" onclick=\"loadConfig()\">Revert</button>\n";
    html << "            <button class=\"btn danger\" onclick=\"restartServer()\">Full Process Restart</button>\n";
    html << "        </div>\n";
    html << "        \n";
    html << "        <div id=\"status\"></div>\n";
    html << "        \n";
    html << "        <div class=\"advanced-toggle\">\n";
    html << "            <button class=\"btn secondary\" onclick=\"toggleAdvanced()\">Show Advanced (Raw INI)</button>\n";
    html << "        </div>\n";
    html << "        \n";
    html << "        <div class=\"advanced-config\" id=\"advancedConfig\">\n";
    html << "            <div class=\"config-panel\">\n";
    html << "                <div class=\"panel-header\">Advanced Configuration (Raw INI File)</div>\n";
    html << "                <div class=\"panel-body\">\n";
    html << "                    <textarea id=\"rawConfigEditor\" placeholder=\"Loading configuration...\"></textarea>\n";
    html << "                    <div style=\"margin-top: 10px;\">\n";
    html << "                        <button class=\"btn secondary\" onclick=\"saveRawConfig()\">Save Raw Config</button>\n";
    html << "                    </div>\n";
    html << "                </div>\n";
    html << "            </div>\n";
    html << "        </div>\n";
    html << "    </div>\n";
    html << "\n";
    html << "    <script>\n";
    html << "        let currentConfig = {};\n";
    html << "        \n";
    html << "        function showStatus(message, isError) {\n";
    html << "            const statusDiv = document.getElementById('status');\n";
    html << "            statusDiv.className = 'status ' + (isError ? 'error' : 'success');\n";
    html << "            statusDiv.textContent = message;\n";
    html << "            setTimeout(function() { statusDiv.textContent = ''; statusDiv.className = 'status'; }, 5000);\n";
    html << "        }\n";
    html << "        \n";
    html << "        function parseIniConfig(iniContent) {\n";
    html << "            const config = {};\n";
    html << "            const lines = iniContent.split('\\n');\n";
    html << "            let currentSection = '';\n";
    html << "            \n";
    html << "            for (let line of lines) {\n";
    html << "                line = line.trim();\n";
    html << "                if (line.startsWith('[') && line.endsWith(']')) {\n";
    html << "                    currentSection = line.slice(1, -1);\n";
    html << "                    config[currentSection] = config[currentSection] || {};\n";
    html << "                } else if (line.includes('=') && currentSection) {\n";
    html << "                    const [key, value] = line.split('=', 2);\n";
    html << "                    config[currentSection][key.trim()] = value.trim();\n";
    html << "                }\n";
    html << "            }\n";
    html << "            return config;\n";
    html << "        }\n";
    html << "        \n";
    html << "        function generateIniConfig() {\n";
    html << "            let ini = '[wangemu]\\n';\n";
    html << "            ini += 'configversion=1\\n';\n";
    html << "            ini += '[wangemu/config-0]\\n';\n";
    html << "            ini += '[wangemu/config-0/cpu]\\n';\n";
    html << "            ini += 'cpu=' + document.getElementById('cpu').value + '\\n';\n";
    html << "            ini += 'memsize=' + document.getElementById('ram').value + '\\n';\n";
    html << "            ini += 'speed=regulated\\n';\n";
    html << "            ini += '[wangemu/config-0/io/slot-0]\\n';\n";
    html << "            ini += 'addr=0x000\\n';\n";
    html << "            ini += 'type=2236 MXD\\n';\n";
    html << "            ini += '[wangemu/config-0/io/slot-0/cardcfg]\\n';\n";
    html << "            \n";
    html << "            const numTerminals = document.querySelector('input[name=\"numTerminals\"]:checked').value;\n";
    html << "            ini += 'numTerminals=' + numTerminals + '\\n';\n";
    html << "            \n";
    html << "            for (let i = 0; i < 4; i++) {\n";
    html << "                const enabled = document.getElementById('term' + (i+1) + '_enabled').checked;\n";
    html << "                const port = document.getElementById('term' + (i+1) + '_port').value;\n";
    html << "                const baud = document.getElementById('term' + (i+1) + '_baud').value;\n";
    html << "                const flow = document.getElementById('term' + (i+1) + '_flow').checked ? '1' : '0';\n";
    html << "                \n";
    html << "                ini += 'terminal' + i + '_baud_rate=' + baud + '\\n';\n";
    html << "                ini += 'terminal' + i + '_com_port=' + (enabled ? port : '') + '\\n';\n";
    html << "                ini += 'terminal' + i + '_flow_control=0\\n';\n";
    html << "                ini += 'terminal' + i + '_sw_flow_control=' + flow + '\\n';\n";
    html << "            }\n";
    html << "            \n";
    html << "            // Disk controller configuration\n";
    html << "            if (document.getElementById('diskEnabled').checked) {\n";
    html << "                ini += '[wangemu/config-0/io/slot-1]\\n';\n";
    html << "                ini += 'addr=' + document.getElementById('diskAddr').value + '\\n';\n";
    html << "                ini += 'type=' + document.getElementById('diskType').value + '\\n';\n";
    html << "                \n";
    html << "                for (let i = 0; i < 4; i++) {\n";
    html << "                    const diskFile = document.getElementById('disk' + i + 'File').value;\n";
    html << "                    ini += 'filename-' + i + '=' + (diskFile || '') + '\\n';\n";
    html << "                }\n";
    html << "                \n";
    html << "                ini += '[wangemu/config-0/io/slot-1/cardcfg]\\n';\n";
    html << "                ini += 'intelligence=' + document.querySelector('input[name=\"intelligence\"]:checked').value + '\\n';\n";
    html << "                ini += 'numDrives=' + document.querySelector('input[name=\"numDrives\"]:checked').value + '\\n';\n";
    html << "                ini += 'warnMismatch=' + (document.getElementById('warnMismatch').checked ? 'true' : 'false') + '\\n';\n";
    html << "            } else {\n";
    html << "                ini += '[wangemu/config-0/io/slot-1]\\n';\n";
    html << "                ini += 'addr=\\n';\n";
    html << "                ini += 'type=\\n';\n";
    html << "            }\n";
    html << "            \n";
    html << "            // Empty slots 2-7\n";
    html << "            for (let slot = 2; slot <= 7; slot++) {\n";
    html << "                ini += '[wangemu/config-0/io/slot-' + slot + ']\\n';\n";
    html << "                ini += 'addr=\\n';\n";
    html << "                ini += 'type=\\n';\n";
    html << "            }\n";
    html << "            \n";
    html << "            ini += '[wangemu/config-0/misc]\\n';\n";
    html << "            ini += 'disk_realtime=true\\n';\n";
    html << "            ini += 'warnio=' + (document.getElementById('warnInvalidIo').checked ? 'true' : 'false') + '\\n';\n";
    html << "            \n";
    html << "            return ini;\n";
    html << "        }\n";
    html << "        \n";
    html << "        function loadConfigIntoForm(config) {\n";
    html << "            // CPU and RAM\n";
    html << "            if (config['wangemu/config-0/cpu']) {\n";
    html << "                document.getElementById('cpu').value = config['wangemu/config-0/cpu']['cpu'] || '2200MVP-C';\n";
    html << "                document.getElementById('ram').value = config['wangemu/config-0/cpu']['memsize'] || '512';\n";
    html << "            }\n";
    html << "            \n";
    html << "            // Misc settings\n";
    html << "            if (config['wangemu/config-0/misc']) {\n";
    html << "                document.getElementById('warnInvalidIo').checked = config['wangemu/config-0/misc']['warnio'] === 'true';\n";
    html << "            }\n";
    html << "            \n";
    html << "            // Terminal settings\n";
    html << "            if (config['wangemu/config-0/io/slot-0/cardcfg']) {\n";
    html << "                const cardcfg = config['wangemu/config-0/io/slot-0/cardcfg'];\n";
    html << "                const numTerminals = cardcfg['numTerminals'] || '1';\n";
    html << "                document.querySelector('input[name=\"numTerminals\"][value=\"' + numTerminals + '\"]').checked = true;\n";
    html << "                \n";
    html << "                for (let i = 0; i < 4; i++) {\n";
    html << "                    const port = cardcfg['terminal' + i + '_com_port'] || '';\n";
    html << "                    const baud = cardcfg['terminal' + i + '_baud_rate'] || '19200';\n";
    html << "                    const flow = cardcfg['terminal' + i + '_sw_flow_control'] === '1';\n";
    html << "                    \n";
    html << "                    document.getElementById('term' + (i+1) + '_enabled').checked = port !== '';\n";
    html << "                    document.getElementById('term' + (i+1) + '_port').value = port || '/dev/ttyUSB' + i;\n";
    html << "                    document.getElementById('term' + (i+1) + '_baud').value = baud;\n";
    html << "                    document.getElementById('term' + (i+1) + '_flow').checked = flow;\n";
    html << "                }\n";
    html << "            }\n";
    html << "            \n";
    html << "            // Disk controller settings\n";
    html << "            if (config['wangemu/config-0/io/slot-1']) {\n";
    html << "                const diskSlot = config['wangemu/config-0/io/slot-1'];\n";
    html << "                const diskEnabled = diskSlot['type'] && diskSlot['type'] !== '';\n";
    html << "                \n";
    html << "                document.getElementById('diskEnabled').checked = diskEnabled;\n";
    html << "                if (diskEnabled) {\n";
    html << "                    document.getElementById('diskType').value = diskSlot['type'] || '6541';\n";
    html << "                    document.getElementById('diskAddr').value = diskSlot['addr'] || '0x310';\n";
    html << "                    \n";
    html << "                    // Load disk file paths\n";
    html << "                    for (let i = 0; i < 4; i++) {\n";
    html << "                        const diskFile = diskSlot['filename-' + i] || '';\n";
    html << "                        document.getElementById('disk' + i + 'File').value = diskFile;\n";
    html << "                    }\n";
    html << "                }\n";
    html << "            }\n";
    html << "            \n";
    html << "            // Disk controller card configuration\n";
    html << "            if (config['wangemu/config-0/io/slot-1/cardcfg']) {\n";
    html << "                const cardcfg = config['wangemu/config-0/io/slot-1/cardcfg'];\n";
    html << "                const intelligence = cardcfg['intelligence'] || 'smart';\n";
    html << "                const numDrives = cardcfg['numDrives'] || '2';\n";
    html << "                const warnMismatch = cardcfg['warnMismatch'] === 'true';\n";
    html << "                \n";
    html << "                document.querySelector('input[name=\"intelligence\"][value=\"' + intelligence + '\"]').checked = true;\n";
    html << "                document.querySelector('input[name=\"numDrives\"][value=\"' + numDrives + '\"]').checked = true;\n";
    html << "                document.getElementById('warnMismatch').checked = warnMismatch;\n";
    html << "            }\n";
    html << "        }\n";
    html << "        \n";
    html << "        function loadConfig() {\n";
    html << "            fetch('/api/config')\n";
    html << "                .then(function(response) { return response.json(); })\n";
    html << "                .then(function(data) {\n";
    html << "                    if (data.error) {\n";
    html << "                        showStatus('Error: ' + data.error, true);\n";
    html << "                    } else {\n";
    html << "                        currentConfig = parseIniConfig(data.iniContent);\n";
    html << "                        loadConfigIntoForm(currentConfig);\n";
    html << "                        document.getElementById('rawConfigEditor').value = data.iniContent;\n";
    html << "                        showStatus('Configuration loaded successfully');\n";
    html << "                    }\n";
    html << "                })\n";
    html << "                .catch(function(error) {\n";
    html << "                    showStatus('Error loading configuration: ' + error, true);\n";
    html << "                });\n";
    html << "        }\n";
    html << "        \n";
    html << "        function saveConfig() {\n";
    html << "            const iniContent = generateIniConfig();\n";
    html << "            const payload = JSON.stringify({ iniContent: iniContent });\n";
    html << "            \n";
    html << "            fetch('/api/config', {\n";
    html << "                method: 'POST',\n";
    html << "                headers: { 'Content-Type': 'application/json' },\n";
    html << "                body: payload\n";
    html << "            })\n";
    html << "            .then(function(response) { return response.json(); })\n";
    html << "            .then(function(data) {\n";
    html << "                if (data.error) {\n";
    html << "                    showStatus('Error: ' + data.error, true);\n";
    html << "                } else {\n";
    html << "                    showStatus('Configuration saved successfully');\n";
    html << "                    document.getElementById('rawConfigEditor').value = iniContent;\n";
    html << "                }\n";
    html << "            })\n";
    html << "            .catch(function(error) {\n";
    html << "                showStatus('Error saving configuration: ' + error, true);\n";
    html << "            });\n";
    html << "        }\n";
    html << "        \n";
    html << "        function saveAndApplyConfig() {\n";
    html << "            const iniContent = generateIniConfig();\n";
    html << "            const payload = JSON.stringify({ iniContent: iniContent });\n";
    html << "            \n";
    html << "            // First save the configuration\n";
    html << "            fetch('/api/config', {\n";
    html << "                method: 'POST',\n";
    html << "                headers: { 'Content-Type': 'application/json' },\n";
    html << "                body: payload\n";
    html << "            })\n";
    html << "            .then(function(response) { return response.json(); })\n";
    html << "            .then(function(data) {\n";
    html << "                if (data.error) {\n";
    html << "                    showStatus('Error saving: ' + data.error, true);\n";
    html << "                } else {\n";
    html << "                    showStatus('Configuration saved, applying changes...');\n";
    html << "                    document.getElementById('rawConfigEditor').value = iniContent;\n";
    html << "                    \n";
    html << "                    // Then perform internal restart to apply changes\n";
    html << "                    return fetch('/api/internal-restart', { method: 'POST' });\n";
    html << "                }\n";
    html << "            })\n";
    html << "            .then(function(response) {\n";
    html << "                if (response) {\n";
    html << "                    return response.json();\n";
    html << "                }\n";
    html << "            })\n";
    html << "            .then(function(data) {\n";
    html << "                if (data && data.error) {\n";
    html << "                    showStatus('Error applying configuration: ' + data.error, true);\n";
    html << "                } else if (data) {\n";
    html << "                    showStatus('Configuration applied successfully - system restarted internally!');\n";
    html << "                }\n";
    html << "            })\n";
    html << "            .catch(function(error) {\n";
    html << "                showStatus('Error: ' + error, true);\n";
    html << "            });\n";
    html << "        }\n";
    html << "        \n";
    html << "        function saveRawConfig() {\n";
    html << "            const content = document.getElementById('rawConfigEditor').value;\n";
    html << "            const payload = JSON.stringify({ iniContent: content });\n";
    html << "            \n";
    html << "            fetch('/api/config', {\n";
    html << "                method: 'POST',\n";
    html << "                headers: { 'Content-Type': 'application/json' },\n";
    html << "                body: payload\n";
    html << "            })\n";
    html << "            .then(function(response) { return response.json(); })\n";
    html << "            .then(function(data) {\n";
    html << "                if (data.error) {\n";
    html << "                    showStatus('Error: ' + data.error, true);\n";
    html << "                } else {\n";
    html << "                    showStatus('Raw configuration saved successfully');\n";
    html << "                    loadConfig(); // Reload to update form\n";
    html << "                }\n";
    html << "            })\n";
    html << "            .catch(function(error) {\n";
    html << "                showStatus('Error saving raw configuration: ' + error, true);\n";
    html << "            });\n";
    html << "        }\n";
    html << "        \n";
    html << "        function reloadConfig() {\n";
    html << "            if (!confirm('Reload configuration from INI file? This will apply the saved configuration to the running server without restarting.')) {\n";
    html << "                return;\n";
    html << "            }\n";
    html << "            \n";
    html << "            fetch('/api/reload', { method: 'POST' })\n";
    html << "                .then(function(response) { return response.json(); })\n";
    html << "                .then(function(data) {\n";
    html << "                    if (data.error) {\n";
    html << "                        showStatus('Error: ' + data.error, true);\n";
    html << "                    } else {\n";
    html << "                        showStatus('Configuration reloaded successfully - some changes may require restart to take effect');\n";
    html << "                        loadConfig(); // Refresh the form with current config\n";
    html << "                    }\n";
    html << "                })\n";
    html << "                .catch(function(error) {\n";
    html << "                    showStatus('Error reloading configuration: ' + error, true);\n";
    html << "                });\n";
    html << "        }\n";
    html << "        \n";
    html << "        function restartServer() {\n";
    html << "            if (!confirm('Are you sure you want to restart the terminal server? Active connections will be interrupted.')) {\n";
    html << "                return;\n";
    html << "            }\n";
    html << "            \n";
    html << "            fetch('/api/restart', { method: 'POST' })\n";
    html << "                .then(function(response) { return response.json(); })\n";
    html << "                .then(function(data) {\n";
    html << "                    if (data.error) {\n";
    html << "                        showStatus('Error: ' + data.error, true);\n";
    html << "                    } else {\n";
    html << "                        showStatus('Terminal server is restarting with updated configuration...');\n";
    html << "                    }\n";
    html << "                })\n";
    html << "                .catch(function(error) {\n";
    html << "                    showStatus('Error restarting server: ' + error, true);\n";
    html << "                });\n";
    html << "        }\n";
    html << "        \n";
    html << "        function toggleAdvanced() {\n";
    html << "            const advancedDiv = document.getElementById('advancedConfig');\n";
    html << "            const button = event.target;\n";
    html << "            if (advancedDiv.style.display === 'none' || advancedDiv.style.display === '') {\n";
    html << "                advancedDiv.style.display = 'block';\n";
    html << "                button.textContent = 'Hide Advanced (Raw INI)';\n";
    html << "            } else {\n";
    html << "                advancedDiv.style.display = 'none';\n";
    html << "                button.textContent = 'Show Advanced (Raw INI)';\n";
    html << "            }\n";
    html << "        }\n";
    html << "        \n";
    html << "        // Direct disk operations (like GUI)\n";
    html << "        function insertDisk(slot, drive) {\n";
    html << "            const fileInput = document.getElementById('disk' + drive + 'File');\n";
    html << "            const filename = fileInput.value.trim();\n";
    html << "            if (!filename) {\n";
    html << "                showStatus('Please enter a disk file path first', true);\n";
    html << "                return;\n";
    html << "            }\n";
    html << "            \n";
    html << "            const payload = JSON.stringify({ slot: slot, drive: drive, filename: filename });\n";
    html << "            const statusSpan = document.getElementById('disk' + drive + 'Status');\n";
    html << "            statusSpan.textContent = 'Inserting...';\n";
    html << "            \n";
    html << "            fetch('/api/disk-insert', {\n";
    html << "                method: 'POST',\n";
    html << "                headers: { 'Content-Type': 'application/json' },\n";
    html << "                body: payload\n";
    html << "            })\n";
    html << "            .then(function(response) { return response.json(); })\n";
    html << "            .then(function(data) {\n";
    html << "                if (data.error) {\n";
    html << "                    showStatus('Error inserting disk: ' + data.error, true);\n";
    html << "                    statusSpan.textContent = 'Failed';\n";
    html << "                    statusSpan.style.color = '#ff0000';\n";
    html << "                } else {\n";
    html << "                    showStatus('Disk inserted successfully');\n";
    html << "                    statusSpan.textContent = 'Inserted';\n";
    html << "                    statusSpan.style.color = '#008000';\n";
    html << "                }\n";
    html << "            })\n";
    html << "            .catch(function(error) {\n";
    html << "                showStatus('Error inserting disk: ' + error, true);\n";
    html << "                statusSpan.textContent = 'Failed';\n";
    html << "                statusSpan.style.color = '#ff0000';\n";
    html << "            });\n";
    html << "        }\n";
    html << "        \n";
    html << "        function removeDisk(slot, drive) {\n";
    html << "            if (!confirm('Are you sure you want to remove the disk from drive ' + drive + '?')) {\n";
    html << "                return;\n";
    html << "            }\n";
    html << "            \n";
    html << "            const payload = JSON.stringify({ slot: slot, drive: drive });\n";
    html << "            const statusSpan = document.getElementById('disk' + drive + 'Status');\n";
    html << "            statusSpan.textContent = 'Removing...';\n";
    html << "            \n";
    html << "            fetch('/api/disk-remove', {\n";
    html << "                method: 'POST',\n";
    html << "                headers: { 'Content-Type': 'application/json' },\n";
    html << "                body: payload\n";
    html << "            })\n";
    html << "            .then(function(response) { return response.json(); })\n";
    html << "            .then(function(data) {\n";
    html << "                if (data.error) {\n";
    html << "                    showStatus('Error removing disk: ' + data.error, true);\n";
    html << "                    statusSpan.textContent = 'Failed';\n";
    html << "                    statusSpan.style.color = '#ff0000';\n";
    html << "                } else {\n";
    html << "                    showStatus('Disk removed successfully');\n";
    html << "                    statusSpan.textContent = '';\n";
    html << "                    statusSpan.style.color = '#666';\n";
    html << "                    document.getElementById('disk' + drive + 'File').value = '';\n";
    html << "                }\n";
    html << "            })\n";
    html << "            .catch(function(error) {\n";
    html << "                showStatus('Error removing disk: ' + error, true);\n";
    html << "                statusSpan.textContent = 'Failed';\n";
    html << "                statusSpan.style.color = '#ff0000';\n";
    html << "            });\n";
    html << "        }\n";
    html << "        \n";
    html << "        // Load configuration on page load\n";
    html << "        loadConfig();\n";
    html << "    </script>\n";
    html << "</body>\n";
    html << "</html>\n";
    
    response.body = html.str();
    return response;
}

WebConfigServer::HttpResponse WebConfigServer::serveStaticFile(const std::string& path) {
    HttpResponse response;
    response.status = 404;
    response.body = "Static files not implemented";
    return response;
}

WebConfigServer::HttpRequest WebConfigServer::parseRequest(const std::string& requestData) {
    HttpRequest request;
    std::istringstream stream(requestData);
    std::string line;
    
    // Parse request line
    if (std::getline(stream, line)) {
        std::istringstream lineStream(line);
        std::string path_with_query;
        lineStream >> request.method >> path_with_query;
        
        // Split path and query string
        size_t queryPos = path_with_query.find('?');
        if (queryPos != std::string::npos) {
            request.path = path_with_query.substr(0, queryPos);
            request.query = path_with_query.substr(queryPos + 1);
        } else {
            request.path = path_with_query;
        }
    }
    
    // Parse headers
    while (std::getline(stream, line) && line != "\r" && !line.empty()) {
        size_t colonPos = line.find(':');
        if (colonPos != std::string::npos) {
            std::string key = line.substr(0, colonPos);
            std::string value = line.substr(colonPos + 1);
            
            // Trim whitespace
            while (!key.empty() && std::isspace(key.back())) key.pop_back();
            while (!value.empty() && std::isspace(value.front())) value.erase(0, 1);
            while (!value.empty() && std::isspace(value.back())) value.pop_back();
            
            request.headers[key] = value;
        }
    }
    
    // Read body (remaining data)
    std::string body;
    std::string bodyLine;
    while (std::getline(stream, bodyLine)) {
        if (!body.empty()) body += "\n";
        body += bodyLine;
    }
    request.body = body;
    
    return request;
}

std::string WebConfigServer::formatResponse(const HttpResponse& response) {
    std::ostringstream stream;
    
    stream << "HTTP/1.1 " << response.status;
    switch (response.status) {
        case 200: stream << " OK"; break;
        case 400: stream << " Bad Request"; break;
        case 404: stream << " Not Found"; break;
        case 405: stream << " Method Not Allowed"; break;
        case 500: stream << " Internal Server Error"; break;
        case 501: stream << " Not Implemented"; break;
        default: stream << " Unknown"; break;
    }
    stream << "\r\n";
    
    // Add headers
    stream << "Content-Length: " << response.body.length() << "\r\n";
    stream << "Connection: close\r\n";
    
    for (const auto& header : response.headers) {
        stream << header.first << ": " << header.second << "\r\n";
    }
    
    stream << "\r\n" << response.body;
    
    return stream.str();
}

std::string WebConfigServer::readIniFile() {
    std::ifstream file(m_iniPath);
    if (!file.is_open()) {
        std::cerr << "[WARN] Could not open INI file: " << m_iniPath << "\n";
        return "";
    }
    
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

bool WebConfigServer::writeIniFile(const std::string& content) {
    std::ofstream file(m_iniPath);
    if (!file.is_open()) {
        std::cerr << "[ERROR] Could not write to INI file: " << m_iniPath << "\n";
        return false;
    }
    
    file << content;
    return file.good();
}

bool WebConfigServer::validateIniContent(const std::string& content) {
    // Basic validation - check for required sections
    if (content.find("[wangemu]") == std::string::npos) {
        return false;
    }
    
    // Could add more validation here
    return true;
}