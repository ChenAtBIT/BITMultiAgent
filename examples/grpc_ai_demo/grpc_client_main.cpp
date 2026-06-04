/**
 * @file grpc_client_main.cpp
 * @brief gRPC AI Client - 连接 AI 服务的 gRPC 客户端
 * 
 * 这是项目的核心示例：
 * - 使用 gRPC 连接 AI 服务端
 * - 支持同步和流式查询
 * - 交互式命令行界面
 */

#include "agent_rpc/client/ai_query_client.h"
#include "ai_query.grpc.pb.h"

#include <grpcpp/grpcpp.h>
#include <iostream>
#include <string>
#include <chrono>
#include <cctype>

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using grpc::ClientReader;

/**
 * @brief gRPC AI 客户端
 */
class GrpcAIClient {
public:
    GrpcAIClient(std::shared_ptr<Channel> channel)
        : stub_(agent_communication::AIQueryService::NewStub(channel)) {
    }
    
    // 同步查询
    std::string Query(const std::string& question, const std::string& context_id = "default") {
        agent_communication::AIQueryRequest request;
        request.set_request_id(generateRequestId());
        request.set_question(question);
        request.set_context_id(context_id);
        request.set_timeout_seconds(60);
        
        agent_communication::AIQueryResponse response;
        ClientContext context;
        
        // 设置超时
        auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(60);
        context.set_deadline(deadline);
        
        auto start = std::chrono::steady_clock::now();
        Status status = stub_->Query(&context, request, &response);
        auto end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        
        if (status.ok()) {
            std::cout << "\n[耗时: " << duration.count() << "ms]" << std::endl;
            return response.answer();
        } else {
            return "错误: " + status.error_message();
        }
    }
    
    // 流式查询
    bool QueryStream(const std::string& question, const std::string& context_id = "default") {
        agent_communication::AIQueryRequest request;
        request.set_request_id(generateRequestId());
        request.set_question(question);
        request.set_context_id(context_id);
        request.set_timeout_seconds(60);
        
        ClientContext context;
        auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(60);
        context.set_deadline(deadline);
        
        std::unique_ptr<ClientReader<agent_communication::AIStreamEvent>> reader(
            stub_->QueryStream(&context, request));
        
        agent_communication::AIStreamEvent event;
        std::string full_content;
        
        std::cout << "\nAI: ";
        std::cout.flush();
        
        while (reader->Read(&event)) {
            if (event.event_type() == "partial") {
                std::cout << event.content();
                std::cout.flush();
                full_content += event.content();
            } else if (event.event_type() == "complete") {
                std::cout << std::endl;
                return true;
            } else if (event.event_type() == "error") {
                std::cout << "\n错误: " << event.content() << std::endl;
                return false;
            } else if (event.event_type() == "status") {
                // 状态更新，可以忽略或显示
            }
        }
        
        Status status = reader->Finish();
        if (!status.ok()) {
            std::cout << "\n流式查询失败: " << status.error_message() << std::endl;
            return false;
        }
        
        std::cout << std::endl;
        return true;
    }

private:
    std::string generateRequestId() {
        static int counter = 0;
        return "req_" + std::to_string(++counter) + "_" + 
               std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    }
    
    std::unique_ptr<agent_communication::AIQueryService::Stub> stub_;
};

void printHelp() {
    std::cout << "\n命令:" << std::endl;
    std::cout << "  /help     - 显示帮助" << std::endl;
    std::cout << "  /stream   - 切换流式模式" << std::endl;
    std::cout << "  /context <id> - 切换上下文" << std::endl;
    std::cout << "  /quit     - 退出" << std::endl;
    std::cout << "\n直接输入问题发送给 AI\n" << std::endl;
}

void printUsage(const char* program) {
    std::cout << "用法: " << program << " <SERVER_ADDRESS>" << std::endl;
    std::cout << std::endl;
    std::cout << "参数:" << std::endl;
    std::cout << "  SERVER_ADDRESS - gRPC 服务器地址 (例如: localhost:50051)" << std::endl;
    std::cout << std::endl;
    std::cout << "示例:" << std::endl;
    std::cout << "  " << program << " localhost:50051" << std::endl;
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
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }
    
    std::string server_address = argv[1];
    
    std::cout << "==========================================" << std::endl;
    std::cout << "gRPC AI Client" << std::endl;
    std::cout << "==========================================" << std::endl;
    std::cout << "连接到: " << server_address << std::endl;
    
    // 创建 channel
    auto channel = grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials());
    
    // 检查连接
    auto state = channel->GetState(true);
    if (state == GRPC_CHANNEL_TRANSIENT_FAILURE || state == GRPC_CHANNEL_SHUTDOWN) {
        std::cerr << "无法连接到服务器: " << server_address << std::endl;
        return 1;
    }
    
    GrpcAIClient client(channel);
    
    printHelp();
    
    std::string context_id = "default";
    promptForContextSwitchIfDefault(context_id);
    bool stream_mode = false;
    std::string line;
    
    while (true) {
        std::cout << "[" << context_id << (stream_mode ? "/流式" : "") << "] > ";
        std::getline(std::cin, line);
        
        if (line.empty()) continue;
        
        if (line == "/quit" || line == "/exit" || line == "/q") {
            std::cout << "再见!" << std::endl;
            break;
        }
        
        if (line == "/help" || line == "/h") {
            printHelp();
            continue;
        }
        
        if (line == "/stream" || line == "/s") {
            stream_mode = !stream_mode;
            std::cout << "流式模式: " << (stream_mode ? "开启" : "关闭") << std::endl;
            continue;
        }
        
        if (line.substr(0, 9) == "/context " || line.substr(0, 3) == "/c ") {
            size_t pos = line.find(' ');
            if (pos != std::string::npos) {
                context_id = line.substr(pos + 1);
                std::cout << "切换到上下文: " << context_id << std::endl;
            }
            continue;
        }
        
        // 发送查询
        std::cout << "\n思考中..." << std::endl;
        
        if (stream_mode) {
            client.QueryStream(line, context_id);
        } else {
            std::string response = client.Query(line, context_id);
            std::cout << "\nAI: " << response << "\n" << std::endl;
        }
    }
    
    return 0;
}
