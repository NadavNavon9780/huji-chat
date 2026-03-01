#include "../include/Server.hpp"
#include <iostream>
#include <fstream>
#include <csignal>
#include <cstdlib>
#include <pqxx/pqxx>

// ==========================================
// CONSTANTS & CONFIGURATION
// ==========================================
namespace Config {
    constexpr int DEFAULT_PORT = 8080;
    constexpr int DEFAULT_THREADS = 4;
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

    if (file.is_open()) {
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
    } else {
        std::cout << "[SYSTEM] No " << filename << " found. Proceeding with defaults/env vars.\n";
    }

    // Override with Environment Variables (Cloud-Ready)
    if (const char* env_port = std::getenv("PORT")) {
        config.port = std::stoi(env_port);
        std::cout << "[SYSTEM] Env Var PORT override: " << config.port << "\n";
    }
    if (const char* env_threads = std::getenv("THREADS")) {
        config.threads = std::stoi(env_threads);
        std::cout << "[SYSTEM] Env Var THREADS override: " << config.threads << "\n";
    }

    std::cout << "[SYSTEM] Final config: Port=" << config.port << ", Threads=" << config.threads << "\n";
    return config;
}

// ==========================================
// GLOBAL STATE & SIGNAL HANDLING
// ==========================================

// Global pointer required for the OS-level signal handler to access the server instance
HttpServer* global_server = nullptr;

/**
 * @brief Intercepts OS signals to ensure graceful server shutdown.
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
// DATABASE MANAGEMENT
// ==========================================

/**
 * @brief Initializes the Postgres connection with a retry-loop for resilience.
 * This handles the 'Race Condition' where the server boots faster than the DB.
 */
void init_database() {
    int attempts = 0;
    const int max_attempts = 5;

    while (attempts < max_attempts) {
        try {
            // Fetch DB_URL from environment variables
            const char* db_url = std::getenv("DB_URL");

            // Fallback for local development if the environment variable is missing
            if (!db_url) db_url = "postgresql://user:password@db:5432/huji_chat";

            // Attempt to open a connection to the PostgreSQL container
            pqxx::connection conn(db_url);

            // This ensures the table creation is 'Atomic' (it either happens fully or not at all).
            pqxx::work W(conn);

            // Define the messages table
            W.exec(R"(
                CREATE TABLE IF NOT EXISTS messages (
                    id SERIAL PRIMARY KEY,
                    username TEXT NOT NULL,
                    content TEXT NOT NULL,
                    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
                );
            )");

            // Finalize the transaction
            W.commit();
            std::cout << "[DATABASE] Connected successfully on attempt " << (attempts + 1) << std::endl;
            return;

        } catch ([[maybe_unused]]const std::exception &e) {
            // If connection fails (e.g., DB is still starting), wait and try again.
            attempts++;
            std::cerr << "[DATABASE] Attempt " << attempts << " failed. Retrying in 2s..." << std::endl;

            // Pause the current thread to give the database time to recover/init
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }

    std::cerr << "[ERROR] Could not connect to database after " << max_attempts << " attempts." << std::endl;
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
    struct tm time_struct{};

    localtime_r(&now, &time_struct);
    char time_buffer[80];
    std::strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", &time_struct);

    res.body = "<h1>Server Status</h1>"
               "<p>Current Time: " + std::string(time_buffer) + "</p>"
               "<p>Status: Healthy</p>";
    return res;
}

/**
 * @brief Cloud-native health check endpoint for Load Balancers and Kubernetes.
 */
Response handle_health([[maybe_unused]] const RequestInfo &req) {
    Response res;
    res.status_code = 200;
    res.status_text = "OK";
    res.content_type = "application/json";
    res.body = R"({"status": "healthy"})";
    return res;
}

/**
 * @brief The core Huji-Chat endpoint. Handles message submission to DB and board rendering.
 */
Response handle_chat(const RequestInfo& req) {
    Response res;

    // Get database URL for this specific request
    const char* db_url = std::getenv("DB_URL");
    if (!db_url) db_url = "postgresql://user:password@localhost:5432/huji_chat";

    // handle new messages
    if (req.method == "POST") {
        std::string user = req.params.contains("user") ? req.params.at("user") : "Anonymous";
        std::string msg = req.params.contains("message") ? req.params.at("message") : "";

        if (!msg.empty() && !user.empty()) {
            try {
                // Open a transient connection to save the message
                pqxx::connection conn(db_url);
                pqxx::work W(conn);

                // Parameterized query to prevent SQL injection
                W.exec_params("INSERT INTO messages (username, content) VALUES ($1, $2)", user, msg);
                W.commit();
            } catch (const std::exception &e) {
                std::cerr << "[DB ERROR] Could not save message: " << e.what() << std::endl;
            }
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

    // Inject dynamic messages from the PostgreSQL backend
    try {
        pqxx::connection conn(db_url);
        pqxx::nontransaction N(conn); // 'nontransaction' is faster for pure READ operations

        // Fetch all messages, ordered oldest to newest
        pqxx::result R = N.exec("SELECT username, content, created_at FROM messages ORDER BY created_at ASC");

        for (auto row : R) {
            // Extract data from the row
            std::string u = row[0].as<std::string>();
            std::string m = row[1].as<std::string>();

            // Format the timestamp directly from the database
            std::string ts = row[2].as<std::string>();

            html += "<div class='msg'><div class='msg-header'>";
            html += "<span class='msg-user'>" + u + "</span>";
            html += "<span class='msg-time'>" + ts + "</span></div>";
            html += "<div class='msg-text'>" + m + "</div></div>";
        }
    } catch (const std::exception &e) {
        std::cerr << "[DB ERROR] " << e.what() << std::endl;
        html += "<div class='msg'><div class='msg-text' style='color:red;'>Error loading database messages.</div></div>";
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
                            <li><strong>Network I/O:</strong> Custom HTTP/1.1 parser with Keep-Alive connection persistence.</li>
                            <li><strong>Database:</strong> Centralized PostgreSQL integration via <code>libpqxx</code> driver.</li>
                        </ul>
                    </div>

                    <div class="tech-card">
                        <h3>🛡️ Security & Safety</h3>
                        <ul>
                            <li><strong>SQL Injection Safe:</strong> Utilizing parameterized database queries.</li>
                            <li><strong>Memory Safe:</strong> Strict payload size limits (10MB) prevent buffer overflows.</li>
                            <li><strong>Path Validation:</strong> Built-in protection against Directory Traversal attacks.</li>
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
                            <p>> Connecting to PostgreSQL Database...</p>
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

    // Initialize Postgres on startup to ensure table exists
    init_database();

    // Load server topology settings
    ServerConfig config = load_config(Config::CONF_FILENAME);

    // Initialize and inject config into the server instance
    static HttpServer server(config.port, config.threads);
    global_server = &server;

    // Register API endpoints
    server.add_route("greet", handle_greet);
    server.add_route("status", handle_status);
    server.add_route("chat", handle_chat);
    server.add_route("health", handle_health);

    // Begin blocking accept() loop
    server.start();

    return 0;
}