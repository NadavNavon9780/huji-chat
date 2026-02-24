#ifndef COMMON_HPP
#define COMMON_HPP
#pragma once

#include <string>
#include <map>
#include <functional>

/**
 * @brief Maximum allowed size for an incoming HTTP payload (10 MB).
 * Prevents buffer overflow and denial-of-service (DoS) attacks.
 */
constexpr size_t MAX_PAYLOAD_SIZE = 10485760;

/**
 * @struct RequestInfo
 * @brief Encapsulates all parsed data from an incoming HTTP request.
 */
struct RequestInfo {
    std::string path;                                ///< The requested URI path.
    std::string query;                               ///< The raw query string.
    std::map<std::string, std::string> params;       ///< Parsed key-value pairs from query/body.
    std::string method;                              ///< HTTP method (GET, POST, etc.).
    std::string body;                                ///< The raw request body.
    bool keep_alive = true;                          ///< Connection persistence flag.
};

/**
 * @struct Response
 * @brief Represents an HTTP response to be sent back to the client.
 */
struct Response {
    int status_code = 200;                           ///< HTTP status code (e.g., 200, 404).
    std::string status_text = "OK";                  ///< HTTP status message.
    std::string content_type = "text/html";          ///< MIME type of the payload.
    std::string body;                                ///< The payload data.
    std::map<std::string, std::string> headers;      ///< Additional HTTP headers.
    bool keep_alive = true;                          ///< Connection persistence flag.

    /**
     * @brief Serializes the response object into a valid HTTP-formatted string.
     * @return std::string The formatted HTTP response ready for socket transmission.
     */
    [[nodiscard]] std::string to_string() const;
};

/**
 * @struct Message
 * @brief Represents a single persistent message in the server's message board.
 */
struct Message {
    std::string user;
    std::string text;
    std::string timestamp;
};

/// Alias for callback functions that handle specific HTTP routes.
using RouteHandler = std::function<Response(const RequestInfo&)>;

#endif // COMMON_HPP