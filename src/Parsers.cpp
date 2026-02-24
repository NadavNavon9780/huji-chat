#include "../include/Parsers.hpp"
#include <sstream>
#include <unordered_map>
#include <cctype>

namespace {
    // Kept in an anonymous namespace to restrict linkage to this file only.
    bool ci_char_compare(char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
    }
}

RequestInfo parse_url(const std::string& url) {
    RequestInfo info;

    // 1. Separate Path from Query
    size_t pos = url.find('?');
    if (pos == std::string::npos) {
        info.path = url;
        info.query = "";
    } else {
        info.path = url.substr(0, pos);
        info.query = url.substr(pos + 1);
    }

    // 2. Normalize Path mapping root to index.html
    if (info.path == "/") info.path = "/index.html";
    if (!info.path.empty() && info.path[0] == '/') {
        info.path.erase(0, 1);
    }

    // 3. Parse Query Parameters into the Map
    std::stringstream ss(info.query);
    std::string segment;
    while (std::getline(ss, segment, '&')) {
        size_t eqPos = segment.find('=');
        if (eqPos != std::string::npos) {
            std::string key = segment.substr(0, eqPos);
            std::string value = segment.substr(eqPos + 1);
            info.params[key] = value;
        }
    }
    return info;
}

std::string url_decode(const std::string& str) {
    std::string decoded;
    decoded.reserve(str.length()); // Pre-allocate memory to optimize concatenation

    for (size_t i = 0; i < str.length(); ++i) {
        if (str[i] == '+') {
            decoded += ' ';
        } else if (str[i] == '%' && i + 2 < str.length()) {
            std::string hex = str.substr(i + 1, 2);
            char c = static_cast<char>(std::stoul(hex, nullptr, 16));
            decoded += c;
            i += 2;
        } else {
            decoded += str[i];
        }
    }
    return decoded;
}

void parse_form_body(const std::string& form_body, RequestInfo& info) {
    std::stringstream ss(form_body);
    std::string segment;

    while (std::getline(ss, segment, '&')) {
        size_t eqPos = segment.find('=');
        if (eqPos != std::string::npos) {
            std::string key = url_decode(segment.substr(0, eqPos));
            std::string value = url_decode(segment.substr(eqPos + 1));
            info.params[key] = value;
        }
    }
}

void parse_json_body(const std::string& body, RequestInfo& info) {
    enum State { SEARCHING, KEY, VALUE };
    State state = SEARCHING;

    std::string currentKey, currentValue;

    for (size_t i = 0; i < body.length(); ++i) {
        char c = body[i];

        switch (state) {
            case SEARCHING:
                if (c == '"') state = KEY;
                break;

            case KEY:
                if (c == '"') state = VALUE;
                else currentKey += c;
                break;

            case VALUE:
                // Ignore leading colons and whitespace
                if (c == ':' || std::isspace(static_cast<unsigned char>(c))) continue;

                // End of value pair
                if (c == ',' || c == '}') {
                    if (!currentKey.empty()) {
                        info.params[currentKey] = currentValue;
                        currentKey.clear();
                        currentValue.clear();
                    }
                    state = SEARCHING;
                }
                // Handle string values and escaped quotes
                else if (c == '"') {
                    i++;
                    while (i < body.length()) {
                        if (body[i] == '\\' && i + 1 < body.length() && body[i+1] == '"') {
                            currentValue += '"';
                            i += 2;
                        } else if (body[i] == '"') {
                            break;
                        } else {
                            currentValue += body[i++];
                        }
                    }
                }
                else {
                    currentValue += c; // Numeric or boolean values
                }
                break;
        }
    }
}

std::string get_mime_type(const std::string& path) {
    // Static map initialized once for O(1) lookups
    static const std::unordered_map<std::string, std::string> mime_types = {
        {".html", "text/html"},
        {".css",  "text/css"},
        {".js",   "application/javascript"},
        {".jpg",  "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".png",  "image/png"}
    };

    size_t dot_pos = path.find_last_of('.');
    if (dot_pos == std::string::npos) return "text/plain";

    std::string ext = path.substr(dot_pos);
    auto it = mime_types.find(ext);

    if (it != mime_types.end()) {
        return it->second;
    }

    return "text/plain"; // Default fallback
}

std::string extract_header_value(const std::string& full_data, size_t max_pos, const std::string& target) {
    // Cast max_pos to the proper signed difference type to satisfy Clang-Tidy
    auto header_end_it = full_data.begin() + static_cast<std::string::difference_type>(max_pos);

    // Perform a case-insensitive search for the target header
    auto it = std::search(full_data.begin(), header_end_it, target.begin(), target.end(), ci_char_compare);

    if (it == header_end_it) return "";

    std::advance(it, target.length());

    // Skip spaces and colons separating the key and value
    while (it != header_end_it && (*it == ' ' || *it == ':')) {
        ++it;
    }

    auto line_end = std::find(it, header_end_it, '\r');
    return {it, line_end};
}