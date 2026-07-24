#include "version.h"
#include "popl.hpp"
#include "StdioTransport.h"
#include "SseTransport.h"
#include "server/Server.h"
#include "tool/ToolRegistry.h"
#include "aixlog.hpp"
#include "utils/MCPBuilder.h"

#include <chrono>
#include <csignal>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>

using namespace popl;
using json = nlohmann::json;

namespace {

std::shared_ptr<vx::mcp::Server> global_server;

void stop_handler(sig_atomic_t) {
    if (global_server && global_server->IsValid()) global_server->Stop();
}

json invalid_params(const json& request, const std::string& message) {
    std::string id;
    if (request.contains("id")) {
        if (request["id"].is_string()) id = request["id"].get<std::string>();
        else id = request["id"].dump();
    }
    return MCPBuilder::Error(MCPBuilder::InvalidParams, id, message);
}

}  // namespace

int main(int argc, char** argv) {
    std::string name;
    std::string plugins_directory;
    std::string logs_directory;
    std::string tool_config;
    std::string sessions_root;
    bool verbose = false;

    OptionParser options("Allowed options");
    auto help = options.add<Switch>("", "help", "produce help message");
    auto name_option = options.add<Value<std::string>>("n", "name", "server name", "mcp-server");
    auto plugins_option = options.add<Value<std::string>>("p", "plugins", "plugin directory", "./plugins");
    auto logs_option = options.add<Value<std::string>>("l", "logs", "log directory", "./logs");
    auto config_option = options.add<Value<std::string>>("c", "tool-config", "tool orchestration config", "./tool_orchestration.json");
    auto sessions_option = options.add<Value<std::string>>("", "sessions-root", "session root", "./sessions");
    auto verbose_option = options.add<Value<bool>>("v", "verbose", "verbose logging", verbose);
    auto sse = options.add<Switch>("s", "sse", "start SSE transport");
    name_option->assign_to(&name);
    plugins_option->assign_to(&plugins_directory);
    logs_option->assign_to(&logs_directory);
    config_option->assign_to(&tool_config);
    sessions_option->assign_to(&sessions_root);
    verbose_option->assign_to(&verbose);

    try {
        options.parse(argc, argv);
        if (help->count()) {
            std::cout << options << std::endl;
            return 0;
        }
    } catch (const std::exception& exception) {
        std::cerr << "Invalid arguments: " << exception.what() << std::endl;
        return 2;
    }

    std::error_code ec;
    std::filesystem::create_directories(logs_directory, ec);
    if (ec) {
        std::cerr << "Cannot create log directory: " << ec.message() << std::endl;
        return 2;
    }
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::ostringstream stamp;
    stamp << std::put_time(std::gmtime(&time), "%Y-%m-%dT%H-%M-%S");
    auto sink = std::make_shared<AixLog::SinkFile>(AixLog::Severity::trace,
        (std::filesystem::path(logs_directory) / ("mcp-server_" + stamp.str() + ".log")).string());
    AixLog::Log::init({sink});

    auto registry = std::make_shared<vx::mcp::ToolRegistry>();
    registry->set_sessions_root(std::filesystem::absolute(sessions_root));
    std::string registry_error;
    if (!registry->Initialize(plugins_directory, tool_config, &registry_error)) {
        std::cerr << "Tool registry initialization failed: " << registry_error << std::endl;
        return 3;
    }
    auto gateway = std::make_shared<vx::mcp::ToolGateway>(*registry);

    global_server = std::make_shared<vx::mcp::Server>();
    global_server->Name(name);
    global_server->VerboseLevel(verbose ? 1 : 0);
    global_server->OverrideCallback("tools/list", [gateway](const json& request) {
        vx::mcp::ToolExecutionContext context;
        std::string error;
        const auto params = request.value("params", json::object());
        if (!vx::mcp::ToolGateway::ParseContext(params, &context, &error)) {
            return invalid_params(request, error);
        }
        auto response = MCPBuilder::Response(request);
        response["result"]["tools"] = gateway->List(context);
        return response;
    });
    global_server->OverrideCallback("tools/call", [gateway](const json& request) {
        vx::mcp::ToolExecutionContext context;
        std::string error;
        const auto params = request.value("params", json::object());
        if (!vx::mcp::ToolGateway::ParseContext(params, &context, &error)) {
            return invalid_params(request, error);
        }
        if (!params.contains("name") || !params["name"].is_string() ||
            !params.contains("arguments") || !params["arguments"].is_object()) {
            return invalid_params(request, "name and arguments object are required");
        }
        auto response = MCPBuilder::Response(request);
        response["result"] = gateway->Call(context, params["name"].get<std::string>(),
                                            params["arguments"]);
        return response;
    });

    std::shared_ptr<vx::ITransport> transport = sse->is_set()
        ? std::static_pointer_cast<vx::ITransport>(std::make_shared<vx::transport::SSE>())
        : std::static_pointer_cast<vx::ITransport>(std::make_shared<vx::transport::Stdio>());
    std::signal(SIGINT, stop_handler);
    LOG(INFO) << "Starting mcp-server v" << PROJECT_VERSION
              << " with tool orchestration config " << tool_config << std::endl;
    return global_server->Connect(transport) ? 0 : 1;
}
