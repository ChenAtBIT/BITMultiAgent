#include "ai_query.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <string>

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;

namespace {

// 这个文件在整套教程里的角色：
// - 连接教程里的 gRPC 桥接服务端。
// - 发送和直连 MCP 客户端几乎相同的教学命令。
// - 让学习者直接比较 unary 和 streaming 两种 gRPC 调用方式。

// 一个尽量小的交互式客户端。
// 代码越短，越容易把注意力放在 unary / streaming 的差异上。
class TutorialClient {
public:
    explicit TutorialClient(std::shared_ptr<Channel> channel)
        : stub_(agent_communication::AIQueryService::NewStub(channel)) {
    }

    // 在普通 RPC 和流式 RPC 之间切换，但对上层调用者保持同样的接口。
    std::string query(const std::string& text, bool stream_mode) {
        return stream_mode ? queryStream(text) : querySync(text);
    }

private:
    // 最普通的一问一答式 gRPC 调用。
    // 对新手来说最容易理解，因为“一行输入”直接对应“一次完整响应”。
    std::string querySync(const std::string& text) {
        agent_communication::AIQueryRequest request;
        request.set_request_id("tutorial-sync");
        request.set_question(text);
        request.set_context_id("tutorial");
        request.set_timeout_seconds(30);

        agent_communication::AIQueryResponse response;
        ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(30));

        Status status = stub_->Query(&context, request, &response);
        if (!status.ok()) {
            return "gRPC 调用失败：" + status.error_message();
        }

        return response.answer();
    }

    // 流式 gRPC 调用。
    // 服务端会先发多个 "partial" 块，再发一个 "complete" 事件，
    // 所以这里需要边读边拼接；如果服务端没有发 partial，只发了 complete，
    // 也仍然能从最后一个事件里拿到完整答案。
    std::string queryStream(const std::string& text) {
        agent_communication::AIQueryRequest request;
        request.set_request_id("tutorial-stream");
        request.set_question(text);
        request.set_context_id("tutorial");
        request.set_timeout_seconds(30);

        ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(30));
        std::unique_ptr<grpc::ClientReader<agent_communication::AIStreamEvent>> reader(
            stub_->QueryStream(&context, request));

        agent_communication::AIStreamEvent event;
        std::string full_text;

        while (reader->Read(&event)) {
            if (event.event_type() == "partial") {
                full_text += event.content();
            } else if (event.event_type() == "complete" && full_text.empty()) {
                // 避免把同一份答案打印两次：
                // 如果前面已经收过 partial，这里的 complete 更像是“结束标记”；
                // 只有在没收到 partial 的情况下，才把它当作最终正文。
                full_text = event.content();
            }
        }

        Status status = reader->Finish();
        if (!status.ok()) {
            return "gRPC 流式调用失败：" + status.error_message();
        }

        return full_text;
    }

    std::unique_ptr<agent_communication::AIQueryService::Stub> stub_;
};

void printHelp(const char* program) {
    std::cout << "gRPC 教学客户端\n\n"
              << "用法：\n"
              << "  " << program << " <server_address>\n\n"
              << "交互命令：\n"
              << "  /help     查看帮助\n"
              << "  /stream   切换流式模式\n"
              << "  /quit     退出\n\n"
              << "可发送给服务端的教学命令：\n"
              << "  /tools\n"
              << "  /schema calculator\n"
              << "  calc 1+7\n"
              << "  /call calculator {\"expression\":\"12*(3+4)\"}\n"
              << "  echo hello grpc mcp\n"
              << std::endl;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printHelp(argv[0]);
        return 1;
    }

    const std::string server_address = argv[1];
    // 教程先使用本地明文连接，目的是先把请求流转看清楚，
    // 不让证书/TLS 配置分散注意力。
    auto channel = grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials());
    TutorialClient client(channel);

    bool stream_mode = false;
    std::string line;

    std::cout << "已连接到教学服务端：" << server_address << "\n" << std::endl;
    printHelp(argv[0]);

    while (true) {
        std::cout << "[grpc-mcp" << (stream_mode ? "/stream" : "") << "] > ";
        if (!std::getline(std::cin, line)) {
            std::cout << std::endl;
            break;
        }

        if (line.empty()) {
            continue;
        }

        if (line == "/quit" || line == "/exit") {
            break;
        }

        if (line == "/help") {
            printHelp(argv[0]);
            continue;
        }

        if (line == "/stream") {
            // 不用重启程序，就能切换成流式模式继续发同样的命令，
            // 方便直接对比两种调用方式的表现。
            stream_mode = !stream_mode;
            std::cout << "流式模式："
                      << (stream_mode ? "已开启" : "已关闭")
                      << "\n" << std::endl;
            continue;
        }

        const std::string answer = client.query(line, stream_mode);
        std::cout << "\n" << answer << "\n" << std::endl;
    }

    return 0;
}
