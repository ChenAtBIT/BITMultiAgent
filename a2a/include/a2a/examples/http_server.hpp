#pragma once

#include <string>
#include <functional>
#include <map>
#include <thread>
#include <atomic>
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <sstream>
#include <cctype>

/**
 * @brief 简单的 HTTP 服务器
 * 用于接收 A2A 协议的 HTTP 请求
 * 支持普通请求和 SSE 流式响应
 */
class HttpServer {
public:
    using RequestHandler = std::function<std::string(const std::string&)>;
    // 流式处理器: 接收请求体和写入回调函数
    using StreamHandler = std::function<void(const std::string&, std::function<bool(const std::string&)>)>;
    
    explicit HttpServer(int port) : port_(port), running_(false), server_fd_(-1) {}
    
    ~HttpServer() {
        stop();
    }
    
    void register_handler(const std::string& path, RequestHandler handler) {
        handlers_[path] = handler;
    }
    
    /**
     * @brief 注册流式处理器
     * @param path 请求路径
     * @param handler 流式处理函数，接收请求体和写入回调
     */
    void register_stream_handler(const std::string& path, StreamHandler handler) {
        stream_handlers_[path] = handler;
    }

    void start() {
        listen();
        serve();
    }

    void listen() {
        if (server_fd_.load() >= 0) {
            return;
        }

        int server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0) {
            throw std::runtime_error("Failed to create socket");
        }

        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(port_);

        if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
            close(server_fd);
            throw std::runtime_error("Failed to bind to port " + std::to_string(port_));
        }

        if (::listen(server_fd, 10) < 0) {
            close(server_fd);
            throw std::runtime_error("Failed to listen on port " + std::to_string(port_));
        }

        server_fd_.store(server_fd);
        running_.store(true);
        std::cout << "HTTP Server listening on port " << port_ << std::endl;
    }

    void serve() {
        if (server_fd_.load() < 0) {
            listen();
        }

        // 接受连接
        while (running_.load()) {
            int server_fd = server_fd_.load();
            if (server_fd < 0) {
                break;
            }

            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            
            int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
            if (client_fd < 0) {
                if (!running_.load()) {
                    break;
                }
                continue;
            }
            
            // 处理请求（在新线程中）
            std::thread([this, client_fd]() {
                this->handle_client(client_fd);
            }).detach();
        }

        close_server_socket();
        running_.store(false);
    }
    
    void stop() {
        running_.store(false);
        close_server_socket();
    }

private:
    /**
     * @brief 从 HTTP 头中解析 Content-Length
     * @param header_text 完整请求头
     * @return 正文长度；未提供时返回 0
     */
    static size_t parse_content_length(const std::string& header_text) {
        std::istringstream header_stream(header_text);
        std::string line;

        while (std::getline(header_stream, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            std::string lower_line = line;
            for (char& ch : lower_line) {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }

            const std::string prefix = "content-length:";
            if (lower_line.rfind(prefix, 0) != 0) {
                continue;
            }

            std::string value_text = line.substr(prefix.size());
            while (!value_text.empty() &&
                   std::isspace(static_cast<unsigned char>(value_text.front()))) {
                value_text.erase(value_text.begin());
            }

            if (value_text.empty()) {
                return 0;
            }

            return static_cast<size_t>(std::stoull(value_text));
        }

        return 0;
    }

    void close_server_socket() {
        int server_fd = server_fd_.exchange(-1);
        if (server_fd >= 0) {
            shutdown(server_fd, SHUT_RDWR);
            close(server_fd);
        }
    }

    void handle_client(int client_fd) {
        std::string request;
        request.reserve(8192);

        char buffer[8192] = {0};
        size_t header_end = std::string::npos;
        size_t content_length = 0;

        while (true) {
            const ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer));
            if (bytes_read <= 0) {
                close(client_fd);
                return;
            }

            request.append(buffer, static_cast<size_t>(bytes_read));

            if (header_end == std::string::npos) {
                header_end = request.find("\r\n\r\n");
                if (header_end != std::string::npos) {
                    // 先完整读到请求头，再根据 Content-Length 继续接收正文。
                    const std::string header_text = request.substr(0, header_end + 4);
                    content_length = parse_content_length(header_text);
                }
            }

            if (header_end != std::string::npos) {
                const size_t body_start = header_end + 4;
                if (request.size() >= body_start + content_length) {
                    break;
                }
            }
        }
        
        // 解析 HTTP 请求
        std::istringstream request_stream(request);
        std::string method, path, version;
        request_stream >> method >> path >> version;
        
        // 提取请求体
        std::string body;
        if (header_end != std::string::npos) {
            const size_t body_pos = header_end + 4;
            body = request.substr(body_pos, content_length);
        }
        
        // 检查是否需要流式响应（通过检查请求体中的 method）
        bool is_stream_request = false;
        if (!body.empty()) {
            // 简单检查是否包含 message/stream 方法
            is_stream_request = (body.find("\"message/stream\"") != std::string::npos);
        }
        
        // 优先检查流式处理器
        auto stream_it = stream_handlers_.find(path);
        if (is_stream_request && stream_it != stream_handlers_.end()) {
            handle_stream_request(client_fd, body, stream_it->second);
            return;
        }

        // 查找普通处理器
        std::string response_body;
        int status_code = 200;
        
        auto it = handlers_.find(path);
        if (it != handlers_.end()) {
            try {
                response_body = it->second(body);
            } catch (const std::exception& e) {
                status_code = 500;
                response_body = std::string("{\"error\":\"") + e.what() + "\"}";
            }
        } else {
            status_code = 404;
            response_body = "{\"error\":\"Not Found\"}";
        }
        
        // 构造 HTTP 响应
        std::ostringstream response;
        response << "HTTP/1.1 " << status_code << " OK\r\n";
        response << "Content-Type: application/json\r\n";
        response << "Content-Length: " << response_body.length() << "\r\n";
        response << "Access-Control-Allow-Origin: *\r\n";
        response << "\r\n";
        response << response_body;
        
        std::string response_str = response.str();
        write(client_fd, response_str.c_str(), response_str.length());
        
        close(client_fd);
    }
    
    /**
     * @brief 处理流式请求 (SSE - Server-Sent Events)
     */
    void handle_stream_request(int client_fd, const std::string& body, StreamHandler& handler) {
        // 发送 SSE 响应头
        std::ostringstream header;
        header << "HTTP/1.1 200 OK\r\n";
        header << "Content-Type: text/event-stream\r\n";
        header << "Cache-Control: no-cache\r\n";
        header << "Connection: keep-alive\r\n";
        header << "Access-Control-Allow-Origin: *\r\n";
        header << "\r\n";
        
        std::string header_str = header.str();
        if (write(client_fd, header_str.c_str(), header_str.length()) < 0) {
            close(client_fd);
            return;
        }
        
        // 调用流式处理器，传入写入回调
        try {
            handler(body, [client_fd](const std::string& event_data) -> bool {
                // 格式化为 SSE 事件
                std::string sse_event = "data: " + event_data + "\n\n";
                ssize_t written = write(client_fd, sse_event.c_str(), sse_event.length());
                return written > 0;
            });
        } catch (const std::exception& e) {
            // 发送错误事件
            std::string error_event = "data: {\"error\":\"" + std::string(e.what()) + "\"}\n\n";
            write(client_fd, error_event.c_str(), error_event.length());
        }
        
        close(client_fd);
    }

    int port_;
    std::atomic<bool> running_;
    std::atomic<int> server_fd_;
    std::map<std::string, RequestHandler> handlers_;
    std::map<std::string, StreamHandler> stream_handlers_;
};
