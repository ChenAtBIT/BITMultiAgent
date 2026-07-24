#pragma once

#include "agent_rpc/mcp/mcp_client.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace agent_rpc::mcp {

struct MCPAgentConfig {
    std::string mcp_server_path;
    std::vector<std::string> mcp_args;
    bool enable_mcp = false;
    int connection_timeout_ms = 5000;
    int tool_call_timeout_ms = 65000;
    int max_retry_count = 1;
    int retry_delay_ms = 250;
};

struct ToolCallResult {
    bool success = false;
    std::string result;
    std::string error;
    std::int64_t duration_ms = 0;
};

struct ToolInfo {
    std::string name;
    std::string tool_id;
    std::string plugin_id;
    std::string description;
    std::string input_schema;
    std::string output_schema;
};

class MCPAgentIntegration {
public:
    MCPAgentIntegration() = default;
    ~MCPAgentIntegration();
    MCPAgentIntegration(const MCPAgentIntegration&) = delete;
    MCPAgentIntegration& operator=(const MCPAgentIntegration&) = delete;

    bool initialize(const MCPAgentConfig& config);
    void shutdown();
    bool isInitialized() const { return initialized_; }
    bool isAvailable() const;

    std::vector<ToolInfo> getAvailableTools(const ToolExecutionContext& context) const;
    ToolCallResult callTool(const ToolExecutionContext& context,
                            const std::string& tool_name,
                            const std::string& arguments);

    const MCPAgentConfig& getConfig() const { return config_; }
    const std::string& getMCPServerPath() const { return config_.mcp_server_path; }
    std::string getStatusDescription() const;

    static std::string toFunctionCallingFormat(const std::vector<ToolInfo>& tools);

private:
    MCPAgentConfig config_;
    std::shared_ptr<MCPClient> mcp_client_;
    std::shared_ptr<MCPToolManager> tool_manager_;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> connected_{false};
};

MCPAgentConfig parseMCPConfigFromArgs(int argc, char* argv[]);
MCPAgentConfig parseMCPConfigFromEnv();

}  // namespace agent_rpc::mcp
