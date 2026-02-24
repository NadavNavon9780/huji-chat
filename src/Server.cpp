#include "../include/Server.hpp"
#include "../include/Parsers.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <iostream>
#include <fstream>
#include <csignal>

// ==========================================
// SERVER CONSTANTS & CONFIGURATION
// ==========================================
namespace ServerConstants {
	constexpr int LISTEN_BACKLOG = 100;        ///< Maximum length of the queue of pending connections
	constexpr int TIMEOUT_SECONDS = 5;         ///< Keep-Alive timeout to prevent thread starvation
	constexpr size_t MAX_READ_BUFFER = 30000;  ///< Initial buffer size for incoming HTTP requests
	constexpr size_t CHUNK_BUFFER_SIZE = 4096; ///< Buffer size for reading large payloads

	const std::string HTTP_DELIM = "\r\n\r\n";
	const std::string PUBLIC_DIR = "public/";
	const std::string DEFAULT_INDEX = "index.html";
	const std::string HDR_CONNECTION = "Connection:";
	const std::string HDR_CONTENT_LEN = "Content-Length:";
	const std::string HDR_CONTENT_TYPE = "Content-Type:";
	const std::string MIME_URLENCODED = "application/x-www-form-urlencoded";
	const std::string MIME_JSON = "application/json";
}

// Global logger mutex ensures console output isn't garbled by concurrent threads
std::mutex log_mutex;

void log_request( const RequestInfo &req, const Response &res )
{
	std::lock_guard <std::mutex> lock(log_mutex);
	std::time_t now = std::time(nullptr);
	struct tm time_struct{};
	localtime_r(&now, &time_struct);
	char time_str[20];
	std::strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &time_struct);

	std::cout << "[" << time_str << "] "
			<< req.method << " " << req.path << " -> "
			<< res.status_code << " " << res.status_text << std::endl;
}

HttpServer::HttpServer( int port, int thread_count )
	: port(port), server_fd(-1), thread_count(thread_count), stop_server(false)
{
	// Ignore SIGPIPE to prevent the server from crashing if a client disconnects unexpectedly
	signal(SIGPIPE, SIG_IGN);
}

HttpServer::~HttpServer()
{
	stop();
}

void HttpServer::add_route( const std::string &path, const RouteHandler &handler )
{
	routes[path] = handler;
}

void HttpServer::start()
{
	// 1. Create the main listening socket (IPv4, TCP)
	server_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (server_fd < 0)
	{
		std::cerr << "[ERROR] Failed to create socket." << std::endl;
		return;
	}

	// 2. Prevent the "Address already in use" error if the server is restarted quickly
	int opt = 1;
	setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	// 3. Configure the server address structure to bind to the specified port on any network interface
	struct sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = INADDR_ANY;
	address.sin_port = htons(port);

	if (bind(server_fd, reinterpret_cast <struct sockaddr *>(&address), sizeof(address)) < 0)
	{
		std::cerr << "[ERROR] Bind failed on port " << port << std::endl;
		return;
	}

	// 4. Start listening with a backlog queue of up to 100 pending connections
	listen(server_fd, ServerConstants::LISTEN_BACKLOG);
	std::cout << "Server listening on port " << port << " with " << thread_count << " threads..." << std::endl;

	// 5. Spin up the worker threads ("Consumers") which will block until tasks are added
	for (int i = 0 ; i < thread_count ; ++i)
	{
		thread_pool.emplace_back(&HttpServer::worker_thread, this);
	}

	// 6. The Main Accept Loop (Producer)
	socklen_t addrlen = sizeof(address);
	while (!stop_server.load())
	{
		// Blocks until a new client connects
		int new_socket = accept(server_fd, reinterpret_cast <struct sockaddr *>(&address), &addrlen);

		if (new_socket >= 0)
		{
			// Safely push the new client socket onto the task queue
			{
				std::lock_guard <std::mutex> lock(queue_mutex);
				task_queue.push(new_socket);
			}
			// Wake up one sleeping worker thread to handle this connection
			cv.notify_one();
		}
		else if (stop_server.load())
		{
			// Accept failed because stop() forcefully closed the socket. Break cleanly.
			break;
		}
	}
}

void HttpServer::worker_thread()
{
	while (true)
	{
		int client_socket; {
			std::unique_lock <std::mutex> lock(queue_mutex);
			// Wait until there's a task or the server is shutting down
			cv.wait(lock, [this]
			{
				return !task_queue.empty() || stop_server.load();
			});

			if (stop_server.load() && task_queue.empty())
			{
				return; // Exit thread cleanly
			}

			client_socket = task_queue.front();
			task_queue.pop();
		}
		handle_client(client_socket);
	}
}

void HttpServer::handle_client( int client_socket ) const
{
	// 1. Configure a strict timeout. If a client connects but sends nothing,
	// we drop them after 5 seconds to prevent thread starvation.
	struct timeval timeout{ServerConstants::TIMEOUT_SECONDS, 0};
	setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast <const char *>(&timeout), sizeof(timeout));

	// 2. The Keep-Alive Loop: Process requests until the connection should close
	while (true)
	{
		char buffer[ServerConstants::MAX_READ_BUFFER] = {0};
		long valread = read(client_socket, buffer, sizeof(buffer));

		if (valread <= 0)
			break; // Break loop if client disconnected or timed out

		std::string requestData(buffer, valread);

		// HTTP headers and body are separated by a double CRLF ("\r\n\r\n")
		size_t body_pos = requestData.find(ServerConstants::HTTP_DELIM);
		if (body_pos == std::string::npos)
			break; // Malformed request

		// Extract the HTTP method (e.g., GET, POST) and the raw URI
		size_t first_space = requestData.find(' ');
		size_t second_space = requestData.find(' ', first_space + 1);
		std::string method = requestData.substr(0, first_space);
		std::string rawUrl = requestData.substr(first_space + 1, second_space - (first_space + 1));

		RequestInfo req = parse_url(rawUrl);
		req.method = method;

		// Determine if the client explicitly requested to close the connection
		std::string conn_header = extract_header_value(requestData, body_pos, ServerConstants::HDR_CONNECTION);
		if (conn_header.find("close") != std::string::npos || conn_header.find("Close") != std::string::npos)
		{
			req.keep_alive = false;
		}

		// 3. Payload Handling: Read the request body if Content-Length is provided
		std::string content_len_str = extract_header_value(requestData, body_pos, ServerConstants::HDR_CONTENT_LEN);
		Response res;
		bool error_occurred = false;

		if (!content_len_str.empty())
		{
			size_t content_length = std::stoull(content_len_str);

			// Security constraint: Prevent memory exhaustion attacks
			if (content_length > MAX_PAYLOAD_SIZE)
			{
				res.status_code = 413;
				res.status_text = "Payload Too Large";
				res.content_type = "text/plain";
				res.body = "Payload exceeds limits.";
				error_occurred = true;
			}
			else
			{
				// Read chunks from the socket until the full body is received
				size_t current_body_len = requestData.length() - (body_pos + 4);
				while (current_body_len < content_length)
				{
					char extra_buffer[ServerConstants::CHUNK_BUFFER_SIZE];
					size_t to_read = std::min(static_cast <size_t>(ServerConstants::CHUNK_BUFFER_SIZE),
					                          content_length - current_body_len);
					long extra_read = read(client_socket, extra_buffer, to_read);
					if (extra_read <= 0)
						break;

					requestData.append(extra_buffer, extra_read);
					current_body_len += extra_read;
				}
				req.body = requestData.substr(body_pos + 4, content_length);
			}
		}

		// 4. Request Routing & Execution
		if (!error_occurred)
		{
			// Parse specific body types for POST requests
			if (req.method == "POST")
			{
				std::string contentType = extract_header_value(requestData, body_pos, ServerConstants::HDR_CONTENT_TYPE);
				if (contentType.find(ServerConstants::MIME_URLENCODED) != std::string::npos)
				{
					parse_form_body(req.body, req);
				}
				else if (contentType.find(ServerConstants::MIME_JSON) != std::string::npos)
				{
					parse_json_body(req.body, req);
				}
			}

			// Normalize path for static routing defaults
			if (req.path.empty() || req.path == "/" || req.path == "public" || req.path == ServerConstants::PUBLIC_DIR)
				req.path = ServerConstants::DEFAULT_INDEX;
			if (req.path.size() >= ServerConstants::PUBLIC_DIR.size() && req.path.substr(
				    0, ServerConstants::PUBLIC_DIR.size()) == ServerConstants::PUBLIC_DIR)
				req.path = req.path.substr(ServerConstants::PUBLIC_DIR.size());

			// Dispatch to registered dynamic route, or fallback to file system
			if (routes.contains(req.path))
			{
				res = routes.at(req.path)(req);
			}
			else
			{
				res = handle_static_file(req.path);
			}
		}

		// 5. Response Finalization
		res.keep_alive = req.keep_alive;
		if (res.status_code >= 400)
			res.keep_alive = false; // Force close on server errors

		std::string raw_response = res.to_string();
		const char *data_ptr = raw_response.data();
		size_t total_to_send = raw_response.length();
		size_t total_sent = 0;

		// Robust send loop: Ensures the entire payload is transmitted even if the OS buffers fill up
		while (total_sent < total_to_send)
		{
			long sent = send(client_socket, data_ptr + total_sent, total_to_send - total_sent, 0);
			if (sent <= 0)
				break;
			total_sent += sent;
		}

		log_request(req, res);

		// Terminate Keep-Alive loop if requested by client or forced by an error
		if (!res.keep_alive)
		{
			break;
		}
	}

	// Ensure the file descriptor is released back to the OS
	close(client_socket);
}

Response HttpServer::handle_static_file( const std::string &requested_path )
{
	Response res;

	// Security check: Prevent Directory Traversal attacks (e.g., requesting "../../../etc/passwd")
	if (requested_path.find("..") != std::string::npos)
	{
		res.status_code = 403;
		res.status_text = "Forbidden";
		res.body = "<h1>403 Forbidden: Directory traversal detected</h1>";
		return res;
	}

	std::string safe_path = ServerConstants::PUBLIC_DIR + requested_path;

	// Open file in binary mode, starting at the end ('ate') to easily calculate file size
	std::ifstream file(safe_path, std::ios::binary | std::ios::ate);

	if (file.is_open())
	{
		// Read the entire file efficiently into the response body string
		std::streamsize size = file.tellg();
		file.seekg(0, std::ios::beg);
		res.body.resize(size);
		file.read(&res.body[0], size);

		// Attach the correct MIME type so the browser renders it properly
		res.content_type = get_mime_type(safe_path);
	}
	else
	{
		res.status_code = 404;
		res.status_text = "Not Found";
		res.body = "<h1>404: File Not Found</h1>";
	}

	return res;
}

void HttpServer::stop()
{
	// Determine if we are the first thread to trigger the stop
	bool expected = false;
	if (!stop_server.compare_exchange_strong(expected, true))
	{
		return; // Server is already stopping
	}

	std::cout << "\n[SYSTEM] Initiating graceful shutdown..." << std::endl;

	// 1. Force accept() to unblock
	if (server_fd >= 0)
	{
		close(server_fd);
		server_fd = -1;
	}

	// 2. Wake up all dormant threads
	cv.notify_all();

	// 3. Join threads to prevent orphaned processes
	for (std::thread &worker: thread_pool)
	{
		if (worker.joinable())
		{
			worker.join();
		}
	}

	// 4. Prevent File Descriptor Leaks by clearing pending queue
	std::lock_guard <std::mutex> lock(queue_mutex);
	while (!task_queue.empty())
	{
		close(task_queue.front());
		task_queue.pop();
	}

	std::cout << "[SYSTEM] All threads joined. Server stopped safely." << std::endl;
}
