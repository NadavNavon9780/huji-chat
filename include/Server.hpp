#ifndef SERVER_HPP
#define SERVER_HPP
#pragma once

#include "Common.hpp"
#include <vector>
#include <queue>
#include <thread>
#include <condition_variable>
#include <map>
#include <string>

/**
 * @class HttpServer
 * @brief A multi-threaded HTTP server utilizing a Thread Pool architecture.
 */
class HttpServer {
public:
    /**
     * @brief Constructs the HTTP Server.
     * @param port The port to bind the server to (e.g., 8080).
     * @param thread_count The number of worker threads in the pool.
     */
    HttpServer(int port, int thread_count);

    /**
     * @brief Destructor ensures safe shutdown and resource cleanup.
     */
    ~HttpServer();

    /**
     * @brief Binds the socket and begins the accept() loop. Blocks the calling thread.
     */
    void start();

    /**
     * @brief Registers a custom callback handler for a specific URI path.
     * @param path The URI path (e.g., "/api/data").
     * @param handler The function to execute when the route is hit.
     */
 void add_route(const std::string& path, const RouteHandler& handler);

    /**
     * @brief Gracefully terminates the server, joining all threads and closing sockets.
     */
    void stop();

private:
    int port;
    int server_fd;
    int thread_count;

    // Concurrency Primitives
    std::vector<std::thread> thread_pool;
    std::queue<int> task_queue;                ///< Queue of client socket file descriptors
    std::mutex queue_mutex;                    ///< Protects access to the task_queue
    std::condition_variable cv;                ///< Notifies worker threads of new tasks

    // CRITICAL: Must be atomic to prevent data races during shutdown
    std::atomic<bool> stop_server;

    std::map<std::string, RouteHandler> routes;

    /**
     * @brief The infinite loop executed by each thread in the pool.
     */
    void worker_thread();

    /**
     * @brief Reads the HTTP request, routes it, and sends the response.
     * @param client_socket The file descriptor for the connected client.
     */
    void handle_client(int client_socket) const;

    /**
     * @brief Fallback handler for serving static files from the public/ directory.
     * @param requested_path The parsed URI path.
     * @return Response The HTTP response containing the file payload.
     */
 static Response handle_static_file(const std::string& requested_path);
};

#endif // SERVER_HPP