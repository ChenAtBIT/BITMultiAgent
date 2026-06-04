/**
 * @file client_main.cpp
 * @brief Interactive Client - 交互式客户端
 * 
 * 基于 a2a-cpp/examples/multi_agent_demo/interactive_client.cpp
 */

#include <a2a/models/agent_message.hpp>
#include <a2a/models/message_part.hpp>
#include <a2a/core/jsonrpc_request.hpp>
#include <a2a/core/jsonrpc_response.hpp>

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <string>
#include <cctype>

using namespace a2a;
using json = nlohmann::json;

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    userp->append((char*)contents, size * nmemb);
    return size * nmemb;
}

class A2AClient {
public:
    explicit A2AClient(const std::string& server_url)
        : server_url_(server_url) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }
    
    ~A2AClient() {
        curl_global_cleanup();
    }
    
    std::string send_message(const std::string& text, const std::string& context_id = "default") {
        // 构造 A2A 消息
        json request = {
            {"jsonrpc", "2.0"},
            {"id", std::to_string(++request_id_)},
            {"method", "message/send"},
            {"params", {
                {"message", {
                    {"role", "user"},
                    {"contextId", context_id},
                    {"parts", {{{"kind", "text"}, {"text", text}}}}
                }},
                {"historyLength", 10}
            }}
        };
        
        // 发送请求
        std::string response_body = post(request.dump());
        
        // 解析响应
        try {
            auto response_json = json::parse(response_body);
            
            if (response_json.contains("error")) {
                return "错误: " + response_json["error"]["message"].get<std::string>();
            }
            
            if (response_json.contains("result") &&
                response_json["result"].contains("parts") &&
                !response_json["result"]["parts"].empty()) {
                return response_json["result"]["parts"][0]["text"].get<std::string>();
            }
            
            return "无法解析响应";
            
        } catch (const std::exception& e) {
            return "解析错误: " + std::string(e.what());
        }
    }
    
    std::string get_agent_card() {
        CURL* curl = curl_easy_init();
        if (!curl) return "{}";
        
        std::string url = server_url_ + "/.well-known/agent-card.json";
        std::string response;
        
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
        
        curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        
        return response;
    }

private:
    std::string post(const std::string& body) {
        CURL* curl = curl_easy_init();
        if (!curl) return "";
        
        std::string response;
        
        curl_easy_setopt(curl, CURLOPT_URL, server_url_.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
        
        CURLcode res = curl_easy_perform(curl);
        
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        
        if (res != CURLE_OK) {
            return "{\"error\": {\"message\": \"" + std::string(curl_easy_strerror(res)) + "\"}}";
        }
        
        return response;
    }
    
    std::string server_url_;
    int request_id_ = 0;
};

void print_help() {
    std::cout << "\n命令:" << std::endl;
    std::cout << "  /help     - 显示帮助" << std::endl;
    std::cout << "  /card     - 获取 Agent Card" << std::endl;
    std::cout << "  /context <id> - 切换上下文" << std::endl;
    std::cout << "  /quit     - 退出" << std::endl;
    std::cout << "\n直接输入文本发送消息给 AI\n" << std::endl;
}

/**
 * @brief 去掉字符串首尾空白字符
 * @param input 待处理字符串
 * @return 去除首尾空白后的字符串
 */
std::string trim(const std::string& input) {
    size_t start = 0;
    while (start < input.size() &&
           std::isspace(static_cast<unsigned char>(input[start]))) {
        start++;
    }

    size_t end = input.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(input[end - 1]))) {
        end--;
    }

    return input.substr(start, end - start);
}

/**
 * @brief 将用户输入规范化为小写，便于识别 yes/no
 * @param input 用户输入
 * @return 转为小写后的结果
 */
std::string normalizeAnswer(std::string input) {
    input = trim(input);
    for (char& ch : input) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return input;
}

/**
 * @brief 当上下文仍为 default 时，引导用户切换到独立会话
 * @param context_id 当前上下文 ID，会在用户确认后原地更新
 */
void promptForContextSwitchIfDefault(std::string& context_id) {
    if (context_id != "default") {
        return;
    }

    std::cout << "\n当前上下文 ID 为 default，多客户端同时使用时可能共享聊天上下文。"
              << std::endl;

    while (true) {
        std::cout << "是否切换到当前会话专用的 context_id？[y/N]: ";

        std::string answer;
        if (!std::getline(std::cin, answer)) {
            std::cout << "\n保持上下文: " << context_id << std::endl;
            return;
        }

        const std::string normalized = normalizeAnswer(answer);
        if (normalized.empty() || normalized == "n" || normalized == "no") {
            std::cout << "保持上下文: " << context_id << std::endl;
            return;
        }

        if (normalized != "y" && normalized != "yes") {
            std::cout << "请输入 y/yes 或 n/no。" << std::endl;
            continue;
        }

        while (true) {
            std::cout << "请输入当前会话的 context_id: ";

            std::string new_context_id;
            if (!std::getline(std::cin, new_context_id)) {
                std::cout << "\n保持上下文: " << context_id << std::endl;
                return;
            }

            new_context_id = trim(new_context_id);
            if (new_context_id.empty()) {
                std::cout << "context_id 不能为空，请重新输入。" << std::endl;
                continue;
            }

            if (new_context_id == "default") {
                std::cout << "请输入一个非 default 的 context_id，以避免会话混用。"
                          << std::endl;
                continue;
            }

            // 用户确认后立即切换为当前会话专用上下文。
            context_id = new_context_id;
            std::cout << "切换到上下文: " << context_id << std::endl;
            return;
        }
    }
}

int main(int argc, char* argv[]) {
    std::string server_url = "http://localhost:5000";
    
    if (argc > 1) {
        server_url = argv[1];
    }
    
    std::cout << "AI Agent 交互客户端" << std::endl;
    std::cout << "连接到: " << server_url << std::endl;
    print_help();
    
    A2AClient client(server_url);
    std::string context_id = "default";
    promptForContextSwitchIfDefault(context_id);
    
    std::string line;
    while (true) {
        std::cout << "[" << context_id << "] > ";
        std::getline(std::cin, line);
        
        if (line.empty()) continue;
        
        if (line == "/quit" || line == "/exit") {
            std::cout << "再见!" << std::endl;
            break;
        }
        
        if (line == "/help") {
            print_help();
            continue;
        }
        
        if (line == "/card") {
            std::cout << "\nAgent Card:\n" << client.get_agent_card() << "\n" << std::endl;
            continue;
        }
        
        if (line.substr(0, 9) == "/context ") {
            context_id = line.substr(9);
            std::cout << "切换到上下文: " << context_id << std::endl;
            continue;
        }
        
        // 发送消息
        std::cout << "\n思考中..." << std::endl;
        std::string response = client.send_message(line, context_id);
        std::cout << "\nAI: " << response << "\n" << std::endl;
    }
    
    return 0;
}
