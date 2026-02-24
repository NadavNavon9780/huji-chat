#ifndef PARSERS_HPP
#define PARSERS_HPP
#pragma once

#include "Common.hpp"
#include <string>

/**
 * @brief Parses a raw URL string into path, query, and key-value parameters.
 * @param url The full URL string received in the HTTP request.
 * @return RequestInfo A populated struct with the extracted path and parameters.
 */
RequestInfo parse_url(const std::string& url);

/**
 * @brief Decodes a URL-encoded string (e.g., converts "%20" to space).
 * @param str The encoded string.
 * @return std::string The decoded plain-text string.
 */
std::string url_decode(const std::string& str);

/**
 * @brief Parses an application/x-www-form-urlencoded body into the RequestInfo params.
 * @param form_body The raw body string.
 * @param info The RequestInfo object to populate with parsed parameters.
 */
void parse_form_body(const std::string& form_body, RequestInfo& info);

/**
 * @brief Parses a JSON body into the RequestInfo params using a simple state machine.
 * @param body The raw JSON string.
 * @param info The RequestInfo object to populate with parsed key-value pairs.
 */
void parse_json_body(const std::string& body, RequestInfo& info);

/**
 * @brief Determines the appropriate MIME type based on a file extension.
 * @param path The requested file path.
 * @return std::string The corresponding MIME type (e.g., "text/html").
 */
std::string get_mime_type(const std::string& path);

/**
 * @brief Extracts a specific header's value from the raw HTTP request data.
 * @param full_data The complete raw HTTP request string.
 * @param max_pos The index where the headers section ends.
 * @param target The header key to search for (case-insensitive).
 * @return std::string The header value, or an empty string if not found.
 */
std::string extract_header_value(const std::string& full_data, size_t max_pos, const std::string& target);

#endif // PARSERS_HPP