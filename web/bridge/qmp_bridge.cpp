// Minimal WebSocket to QMP bridge server
// Provides QMP access to the web UI for VA translation and memory queries

#include <iostream>
#include <string>
#include <thread>
#include <map>
#include <mutex>
#include <condition_variable>
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include <json/json.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

typedef websocketpp::server<websocketpp::config::asio> WsServer;
typedef WsServer::message_ptr WsMessagePtr;

class QmpBridge {
public:
    QmpBridge(int wsPort = 8080, const std::string& qmpHost = "127.0.0.1", int qmpPort = 4445)
        : wsPort_(wsPort), qmpHost_(qmpHost), qmpPort_(qmpPort), qmpSocket_(-1) {}

    bool Start() {
        // Connect to QEMU QMP
        if (!ConnectToQmp()) {
            std::cerr << "Failed to connect to QMP at " << qmpHost_ << ":" << qmpPort_ << std::endl;
            return false;
        }

        // Initialize WebSocket server
        try {
            wsServer_.init_asio();
            wsServer_.set_reuse_addr(true);

            // Set up handlers
            wsServer_.set_open_handler([this](websocketpp::connection_hdl hdl) {
                OnWsOpen(hdl);
            });

            wsServer_.set_close_handler([this](websocketpp::connection_hdl hdl) {
                OnWsClose(hdl);
            });

            wsServer_.set_message_handler([this](websocketpp::connection_hdl hdl, WsMessagePtr msg) {
                OnWsMessage(hdl, msg);
            });

            // Listen on WebSocket port
            wsServer_.listen(wsPort_);
            wsServer_.start_accept();

            std::cout << "QMP Bridge listening on ws://localhost:" << wsPort_ << std::endl;
            std::cout << "Forwarding to QMP at " << qmpHost_ << ":" << qmpPort_ << std::endl;

            // Run WebSocket server
            wsServer_.run();
            return true;
        } catch (const std::exception& e) {
            std::cerr << "WebSocket server error: " << e.what() << std::endl;
            return false;
        }
    }

    void Stop() {
        wsServer_.stop();
        if (qmpSocket_ >= 0) {
            close(qmpSocket_);
            qmpSocket_ = -1;
        }
    }

private:
    bool ConnectToQmp() {
        // Create socket
        qmpSocket_ = socket(AF_INET, SOCK_STREAM, 0);
        if (qmpSocket_ < 0) {
            return false;
        }

        // Connect to QMP
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(qmpPort_);
        inet_pton(AF_INET, qmpHost_.c_str(), &addr.sin_addr);

        if (connect(qmpSocket_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(qmpSocket_);
            qmpSocket_ = -1;
            return false;
        }

        // Read QMP greeting
        char buffer[4096];
        recv(qmpSocket_, buffer, sizeof(buffer), 0);

        // Send capabilities negotiation
        const char* caps = "{\"execute\":\"qmp_capabilities\"}\n";
        send(qmpSocket_, caps, strlen(caps), 0);
        recv(qmpSocket_, buffer, sizeof(buffer), 0);

        return true;
    }

    void OnWsOpen(websocketpp::connection_hdl hdl) {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        connections_.insert(hdl);
        std::cout << "WebSocket client connected" << std::endl;
    }

    void OnWsClose(websocketpp::connection_hdl hdl) {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        connections_.erase(hdl);
        std::cout << "WebSocket client disconnected" << std::endl;
    }

    void OnWsMessage(websocketpp::connection_hdl hdl, WsMessagePtr msg) {
        const std::string& payload = msg->get_payload();

        // Parse JSON request
        Json::Value request;
        Json::Reader reader;
        if (!reader.parse(payload, request)) {
            SendWsError(hdl, "Invalid JSON");
            return;
        }

        // Forward to QMP with newline
        std::string qmpCommand = payload + "\n";

        std::lock_guard<std::mutex> lock(qmpMutex_);
        if (qmpSocket_ < 0) {
            SendWsError(hdl, "QMP not connected");
            return;
        }

        // Send to QMP
        if (send(qmpSocket_, qmpCommand.c_str(), qmpCommand.length(), 0) < 0) {
            SendWsError(hdl, "Failed to send to QMP");
            return;
        }

        // Receive QMP response
        char buffer[65536];
        ssize_t received = recv(qmpSocket_, buffer, sizeof(buffer) - 1, 0);
        if (received <= 0) {
            SendWsError(hdl, "Failed to receive from QMP");
            return;
        }

        buffer[received] = '\0';

        // Forward response back to WebSocket client
        try {
            wsServer_.send(hdl, buffer, websocketpp::frame::opcode::text);
        } catch (const std::exception& e) {
            std::cerr << "Failed to send WebSocket response: " << e.what() << std::endl;
        }
    }

    void SendWsError(websocketpp::connection_hdl hdl, const std::string& error) {
        Json::Value response;
        response["error"]["class"] = "BridgeError";
        response["error"]["desc"] = error;

        Json::FastWriter writer;
        std::string json = writer.write(response);

        try {
            wsServer_.send(hdl, json, websocketpp::frame::opcode::text);
        } catch (const std::exception& e) {
            std::cerr << "Failed to send error: " << e.what() << std::endl;
        }
    }

    int wsPort_;
    std::string qmpHost_;
    int qmpPort_;
    int qmpSocket_;

    WsServer wsServer_;
    std::set<websocketpp::connection_hdl, std::owner_less<websocketpp::connection_hdl>> connections_;
    std::mutex connectionsMutex_;
    std::mutex qmpMutex_;
};

int main(int argc, char* argv[]) {
    int wsPort = 8080;
    std::string qmpHost = "127.0.0.1";
    int qmpPort = 4445;

    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--ws-port" && i + 1 < argc) {
            wsPort = std::atoi(argv[++i]);
        } else if (std::string(argv[i]) == "--qmp-host" && i + 1 < argc) {
            qmpHost = argv[++i];
        } else if (std::string(argv[i]) == "--qmp-port" && i + 1 < argc) {
            qmpPort = std::atoi(argv[++i]);
        } else if (std::string(argv[i]) == "--help") {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "Options:\n"
                      << "  --ws-port PORT    WebSocket port (default: 8080)\n"
                      << "  --qmp-host HOST   QMP host (default: 127.0.0.1)\n"
                      << "  --qmp-port PORT   QMP port (default: 4445)\n";
            return 0;
        }
    }

    QmpBridge bridge(wsPort, qmpHost, qmpPort);

    // Handle shutdown
    std::signal(SIGINT, [](int) {
        std::cout << "\nShutting down..." << std::endl;
        exit(0);
    });

    if (!bridge.Start()) {
        return 1;
    }

    return 0;
}