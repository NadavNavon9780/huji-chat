#include "../include/Common.hpp"
#include <sstream>

std::string Response::to_string() const {
	std::ostringstream oss;

	// Build the HTTP status line
	oss << "HTTP/1.1 " << status_code << " " << status_text << "\r\n";

	// Build standard headers
	oss << "Content-Type: " << content_type << "\r\n";
	oss << "Content-Length: " << body.length() << "\r\n";
	oss << "Connection: " << (keep_alive ? "keep-alive" : "close") << "\r\n";

	// Append custom headers
	for (const auto& [key, val] : headers) {
		oss << key << ": " << val << "\r\n";
	}

	// Append the body separated by a blank line
	oss << "\r\n" << body;

	return oss.str();
}