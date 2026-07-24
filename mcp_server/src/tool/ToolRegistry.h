#pragma once

#include "PluginAPI.h"
#include "loader/PluginsLoader.h"
#include "json.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace vx::mcp {

using json = nlohmann::json;

enum class PermissionDecision { Allow, Ask, Deny };

struct ToolExecutionContext {
    std::string session_id;
    std::string operation_id;
    std::string mode_id;
    std::string actor_kind;
    std::string actor_id;
    json trusted_data = json::object();
};

struct RegisteredTool {
    std::string id;
    std::string name;
    std::string plugin_id;
    std::string description;
    json input_schema;
    json output_schema;
    PluginAPI* plugin = nullptr;
};

class PermissionEvaluator {
public:
    virtual ~PermissionEvaluator() = default;
    virtual PermissionDecision Evaluate(const ToolExecutionContext& context,
                                        const RegisteredTool& tool) const = 0;
};

class AllowPermissionEvaluator final : public PermissionEvaluator {
public:
    PermissionDecision Evaluate(const ToolExecutionContext&,
                                const RegisteredTool&) const override {
        return PermissionDecision::Allow;
    }
};

class ToolRegistry {
public:
    bool Initialize(const std::filesystem::path& plugin_directory,
                    const std::filesystem::path& config_path,
                    std::string* error);

    std::vector<RegisteredTool> ListTools(const ToolExecutionContext& context) const;
    const RegisteredTool* Resolve(const std::string& id_or_name) const;
    bool IsAllowed(const ToolExecutionContext& context,
                   const RegisteredTool& tool) const;
    const std::filesystem::path& sessions_root() const { return sessions_root_; }
    void set_sessions_root(std::filesystem::path value) { sessions_root_ = std::move(value); }

private:
    bool LoadConfig(const std::filesystem::path& path, std::string* error);
    bool RegisterPlugins(std::string* error);

    PluginsLoader loader_;
    std::map<std::string, RegisteredTool> tools_by_id_;
    std::map<std::string, std::string> id_by_name_;
    std::map<std::string, std::map<std::string, std::set<std::string>>> mode_actor_plugins_;
    std::set<std::string> configured_plugins_;
    AllowPermissionEvaluator permission_evaluator_;
    std::filesystem::path sessions_root_;
};

class ToolGateway {
public:
    explicit ToolGateway(ToolRegistry& registry) : registry_(registry) {}

    json List(const ToolExecutionContext& context) const;
    json Call(const ToolExecutionContext& context,
              const std::string& tool_name,
              const json& arguments) const;

    static bool ParseContext(const json& params,
                             ToolExecutionContext* context,
                             std::string* error);

private:
    static bool ValidateArguments(const json& schema,
                                  const json& arguments,
                                  std::string* error);
    static bool ValidateValue(const json& schema,
                              const json& value,
                              const std::string& path,
                              std::string* error);

    ToolRegistry& registry_;
};

}  // namespace vx::mcp
