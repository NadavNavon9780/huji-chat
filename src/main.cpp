#include "../include/Server.hpp"
#include <iostream>
#include <fstream>
#include <vector>
#include <csignal>

// ==========================================
// CONSTANTS & CONFIGURATION
// ==========================================
namespace Config {
    constexpr int DEFAULT_PORT = 8080;
    constexpr int DEFAULT_THREADS = 4;
    const std::string DB_FILENAME = "chat_db.txt";
    const std::string CONF_FILENAME = "server.conf";
}

struct ServerConfig {
    int port = Config::DEFAULT_PORT;
    int threads = Config::DEFAULT_THREADS;
};

/**
 * @brief Parses the server configuration file to override default settings.
 * @param filename The path to the configuration file.
 * @return ServerConfig A struct containing the port and thread count.
 */
ServerConfig load_config(const std::string& filename) {
    ServerConfig config;
    std::ifstream file(filename);

    if (!file.is_open()) {
        std::cout << "[SYSTEM] No " << filename << " found. Using default settings.\n";
        return config;
    }

    std::string line;
    while (std::getline(file, line)) {
        size_t delim = line.find('=');
        if (delim != std::string::npos) {
            std::string key = line.substr(0, delim);
            std::string val = line.substr(delim + 1);

            if (key == "port") config.port = std::stoi(val);
            if (key == "threads") config.threads = std::stoi(val);
        }
    }

    std::cout << "[SYSTEM] Loaded config: Port=" << config.port << ", Threads=" << config.threads << "\n";
    return config;
}

// ==========================================
// GLOBAL STATE & SIGNAL HANDLING
// ==========================================

// Global pointer required for the OS-level signal handler to access the server instance
HttpServer* global_server = nullptr;

/**
 * @brief Intercepts OS signals (like Ctrl+C) to ensure graceful server shutdown.
 * @param signum The signal number caught by the OS.
 */
void handle_sigint(int signum) {
    std::cout << "\n[SYSTEM] Caught signal " << signum << " (SIGINT). Shutting down...\n";
    if (global_server) {
        global_server->stop();
    }
    exit(0);
}

// ==========================================
// IN-MEMORY DATABASE (Huji-Chat)
// ==========================================

std::vector<Message> chat_history;
std::mutex chat_mutex; // Protects concurrent access to the chat_history vector and the text file

/**
 * @brief Hydrates the in-memory chat history from the persistent disk file on startup.
 */
void load_database() {
    std::ifstream file(Config::DB_FILENAME);
    if (!file.is_open()) return;

    std::string line;
    // Expected format: "User|DD/MM/YY HH:MM|Message"
    while (std::getline(file, line)) {
        size_t d1 = line.find('|');
        size_t d2 = line.find('|', d1 + 1);

        if (d1 != std::string::npos && d2 != std::string::npos) {
            std::string u = line.substr(0, d1);
            std::string ts = line.substr(d1 + 1, d2 - d1 - 1);
            std::string msg = line.substr(d2 + 1);
            chat_history.push_back({u, msg, ts});
        }
    }
    std::cout << "[SYSTEM] Loaded " << chat_history.size() << " messages from disk." << std::endl;
}

// ==========================================
// ROUTE HANDLERS
// ==========================================

/**
 * @brief Basic greeting endpoint for testing query parameter parsing.
 */
Response handle_greet(const RequestInfo &req) {
    Response res;
    std::string name = req.params.contains("name") ? req.params.at("name") : "Guest";
    res.body = "<h1>Hello, " + name + "!</h1>";
    return res;
}

/**
 * @brief Returns server health and thread-safe timestamp diagnostics.
 */
Response handle_status([[maybe_unused]] const RequestInfo &req) {
    Response res;
    std::time_t now = std::time(nullptr);
    struct tm time_struct{}; // Zero-initialized for memory safety

    localtime_r(&now, &time_struct);
    char time_buffer[80];
    std::strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", &time_struct);

    res.body = "<h1>Server Status</h1>"
               "<p>Current Time: " + std::string(time_buffer) + "</p>"
               "<p>Status: Healthy</p>";
    return res;
}

/**
 * @brief The core Huji-Chat endpoint. Handles both message submission (POST) and board rendering (GET).
 */
Response handle_chat(const RequestInfo& req) {
    Response res;

    // --- 1. HANDLE NEW MESSAGES (POST) ---
    if (req.method == "POST") {
        std::string user = req.params.contains("user") ? req.params.at("user") : "Anonymous";
        std::string msg = req.params.contains("message") ? req.params.at("message") : "";

        if (!msg.empty() && !user.empty()) {
            std::time_t now = std::time(nullptr);
            struct tm time_struct{};
            localtime_r(&now, &time_struct);
            char time_buf[20];
            std::strftime(time_buf, sizeof(time_buf), "%d/%m/%y %H:%M", &time_struct);
            std::string timestamp(time_buf);

            // Critical Section: Writing to memory and disk safely
            std::lock_guard<std::mutex> lock(chat_mutex);
            chat_history.push_back({user, msg, timestamp});

            std::ofstream file(Config::DB_FILENAME, std::ios::app);
            file << user << "|" << timestamp << "|" << msg << "\n";
        }

        // Post/Redirect/Get (PRG) pattern prevents duplicate form submissions
        res.status_code = 303;
        res.status_text = "See Other";
        res.headers["Location"] = "/chat";
        return res;
    }

    // --- 2. RENDER THE WEBPAGE (GET) ---
    std::string html = R"(
    <!DOCTYPE html>
    <html lang="en">
    <head>
        <meta charset="UTF-8">
        <meta name="viewport" content="width=device-width, initial-scale=1.0">
        <title>Huji-Chat | Portfolio Showcase</title>
        <link rel="stylesheet" href="/style.css">
    </head>
    <body>
        <div class="dashboard-container">
            <header class="app-header">
                <h1>Huji-Chat <span>// Multi-Threaded C++ Web Server</span></h1>
            </header>

            <main class="split-view">
                <section class="chat-section">
                    <div class="section-header">
                        <h2>Live Chat Board</h2>
                        <span class="status-indicator"></span>
                    </div>
                    <div id="chat-box">
    )";

    // Inject dynamic messages from the C++ backend
    {
        std::lock_guard<std::mutex> lock(chat_mutex);
        for (const auto& m : chat_history) {
            html += "<div class='msg'><div class='msg-header'>";
            html += "<span class='msg-user'>" + m.user + "</span>";
            html += "<span class='msg-time'>" + m.timestamp + "</span></div>";
            html += "<div class='msg-text'>" + m.text + "</div></div>";
        }
    }

    // Resume the HTML literal for the right column
    html += R"(
                    </div>
                    <form method="POST" action="/chat" class="chat-form">
                        <input type="text" name="user" placeholder="Your Name" required>
                        <textarea name="message" placeholder="Type a message..." required rows="2"></textarea>
                        <button type="submit" class="btn">Send Message</button>
                    </form>
                </section>

                <section class="info-section">
                    <h2>Under the Hood</h2>

                    <div class="tech-card">
                        <h3>⚙️ Core Architecture</h3>
                        <ul>
                            <li><strong>Thread Pool:</strong> Producer-Consumer pattern utilizing <code>std::mutex</code> & <code>std::condition_variable</code>.</li>
                            <li><strong>Network I/O:</strong> Custom HTTP/1.1 parser with Keep-Alive connection persistence and socket timeouts.</li>
                            <li><strong>Routing:</strong> <i>O(1)</i> MIME-type resolution via unordered maps and dynamic callback dispatching.</li>
                        </ul>
                    </div>

                    <div class="tech-card">
                        <h3>🛡️ Security & Safety</h3>
                        <ul>
                            <li><strong>Memory Safe:</strong> Strict payload size limits (10MB) prevent buffer overflows.</li>
                            <li><strong>Path Validation:</strong> Built-in protection against Directory Traversal attacks.</li>
                            <li><strong>Thread Safe:</strong> Atomic flags for graceful shutdown; mutex-locked disk hydration.</li>
                        </ul>
                    </div>

                    <div class="tech-card terminal">
                        <div class="terminal-header">
                            <span class="dot red"></span>
                            <span class="dot yellow"></span>
                            <span class="dot green"></span>
                            server_status.log
                        </div>
                        <div class="terminal-body">
                            <p>> Starting C++ HTTP Server...</p>
                            <p>> Binding to IPv4 0.0.0.0:9090</p>
                            <p>> Spawning Worker Threads...</p>
                            <p>> Database hydrated successfully.</p>
                            <p class="blink">> Server is listening_</p>
                        </div>
                    </div>
                </section>
            </main>
        </div>
    </body>
    </html>
    )";

    res.body = html;
    return res;
}

// ==========================================
// MAIN SERVER ENTRY POINT
// ==========================================

int main() {
    // Register the Ctrl+C signal handler for graceful shutdown
    signal(SIGINT, handle_sigint);

    // Hydrate state from persistent storage
    load_database();

    // Load server topology settings
    ServerConfig config = load_config(Config::CONF_FILENAME);

    // Initialize and inject config into the server instance
    static HttpServer server(config.port, config.threads);
    global_server = &server;

    // Register API endpoints
    server.add_route("greet", handle_greet);
    server.add_route("status", handle_status);
    server.add_route("chat", handle_chat);

    // Begin blocking accept() loop
    server.start();

    return 0;
}