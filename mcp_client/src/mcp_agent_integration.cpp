#include "agent_rpc/mcp/mcp_agent_integration.h"
#include "agent_rpc/common/logger.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <sstream>
#include <thread>

namespace agent_rpc::mcp {

using json = nlohmann::json;

MCPAgentIntegration::~MCPAgentIntegration() { shutdown(); }

bool MCPAgentIntegration::initialize(const MCPAgentConfig& config) {
    if (initialized_) return true;
    config_ = config;
    if (!config_.enable_mcp) {
        initialized_ = true;
        return true;
    }
    if (config_.mcp_server_path.empty()) {
        LOG_ERROR("MCP server path is required when MCP is enabled");
        return false;
    }
    mcp_client_ = std::make_shared<MCPClient>();
    MCPConnectionConfig connection;
    connection.transport = MCPTransportType::STDIO;
    connection.server_path = config_.mcp_server_path;
    connection.server_args = config_.mcp_args;
    connection.connect_timeout_ms = config_.connection_timeout_ms;
    connection.request_timeout_ms = config_.tool_call_timeout_ms;
    if (!mcp_client_->connect(connection)) {
        mcp_client_.reset();
        return false;
    }
    tool_manager_ = std::make_shared<MCPToolManager>(mcp_client_);
    if (!tool_manager_->initialize()) {
        mcp_client_->disconnect();
        tool_manager_.reset();
        mcp_client_.reset();
        return false;
    }
    connected_ = true;
    initialized_ = true;
    return true;
}

void MCPAgentIntegration::shutdown() {
    if (tool_manager_) tool_manager_->shutdown();
    if (mcp_client_) mcp_client_->disconnect();
    tool_manager_.reset();
    mcp_client_.reset();
    connected_ = false;
    initialized_ = false;
}

bool MCPAgentIntegration::isAvailable() const {
    return initialized_ && connected_ && mcp_client_ && mcp_client_->isConnected();
}

std::vector<ToolInfo> MCPAgentIntegration::getAvailableTools(const ToolExecutionContext& context) const {
    std::vector<ToolInfo> result;
    if (!isAvailable() || !tool_manager_) return result;
    for (const auto& tool : tool_manager_->getAvailableTools(context)) {
        result.push_back({tool.name, tool.tool_id, tool.plugin_id, tool.description,
                          tool.input_schema, tool.output_schema});
    }
    return result;
}

ToolCallResult MCPAgentIntegration::callTool(const ToolExecutionContext& context,
                                             const std::string& tool_name,
                                             const std::string& arguments) {
    ToolCallResult result;
    const auto started = std::chrono::steady_clock::now();
    if (!isAvailable() || !tool_manager_) {
        result.error = "MCP is not available";
        return result;
    }
    for (int attempt = 0; attempt <= config_.max_retry_count; ++attempt) {
        const auto response = tool_manager_->executeTool(context, tool_name, arguments);
        if (!response.is_error) {
            result.result = response.result;
            try {
                const auto payload = json::parse(response.result);
                if (payload.value("isError", false)) {
                    result.error = payload.value("structuredContent", json::object())
                        .value("error", json::object()).value("message", "tool execution failed");
                } else {
                    result.success = true;
                    break;
                }
            } catch (...) {
                result.error = "MCP tool returned invalid JSON";
            }
        }
        if (response.is_error) result.error = response.error;
        if (attempt < config_.max_retry_count) {
            std::this_thread::sleep_for(std::chrono::milliseconds(config_.retry_delay_ms));
        }
    }
    result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    return result;
}

std::string MCPAgentIntegration::getStatusDescription() const {
    if (!initialized_) return "Not initialized";
    if (!config_.enable_mcp) return "MCP disabled";
    if (!isAvailable()) return "MCP unavailable";
    return "Connected";
}

std::string MCPAgentIntegration::toFunctionCallingFormat(const std::vector<ToolInfo>& tools) {
    json result = json::array();
    for (const auto& tool : tools) {
        json parameters = json::object();
        try { parameters = json::parse(tool.input_schema); } catch (...) {}
        result.push_back({{"type", "function"},
                          {"function", {{"name", tool.name},
                                        {"description", tool.description},
                                        {"parameters", parameters}}}});
    }
    return result.dump();
}

MCPAgentConfig parseMCPConfigFromArgs(int argc, char* argv[]) {
    MCPAgentConfig config;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--mcp-server" && index + 1 < argc) {
            config.mcp_server_path = argv[++index];
            config.enable_mcp = true;
        } else if (argument == "--mcp-args" && index + 1 < argc) {
            std::stringstream stream(argv[++index]);
            std::string item;
            while (std::getline(stream, item, ',')) if (!item.empty()) config.mcp_args.push_back(item);
        } else if (argument == "--enable-mcp") {
            config.enable_mcp = true;
        } else if (argument == "--mcp-timeout" && index + 1 < argc) {
            config.tool_call_timeout_ms = std::stoi(argv[++index]);
        }
    }
    return config;
}

MCPAgentConfig parseMCPConfigFromEnv() {
    MCPAgentConfig config;
    if (const char* value = std::getenv("MCP_SERVER_PATH"); value && *value) {
        config.mcp_server_path = value;
        config.enable_mcp = true;
    }
    if (const char* value = std::getenv("MCP_SERVER_ARGS"); value && *value) {
        std::stringstream stream(value);
        std::string item;
        while (std::getline(stream, item, ',')) if (!item.empty()) config.mcp_args.push_back(item);
    }
    if (const char* value = std::getenv("MCP_ENABLED")) {
        const std::string enabled = value;
        config.enable_mcp = enabled == "1" || enabled == "true" || enabled == "yes";
    }
    if (const char* value = std::getenv("MCP_TIMEOUT_MS"); value && *value) {
        try { config.tool_call_timeout_ms = std::stoi(value); } catch (...) {}
    }
    return config;
}

}  // namespace agent_rpc::mcp
