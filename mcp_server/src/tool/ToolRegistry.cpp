#include "tool/ToolRegistry.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>

namespace vx::mcp {
namespace {

constexpr std::size_t kMaxPluginResultBytes = 2 * 1024 * 1024;
std::mutex audit_mutex;

bool safe_token(const std::string& value) {
    static const std::regex pattern("^[A-Za-z0-9][A-Za-z0-9_.-]{0,127}$");
    return std::regex_match(value, pattern);
}

bool valid_schema(const json& schema, const std::string& path, std::string* error) {
    if (!schema.is_object()) {
        if (error) *error = path + " must be a JSON object";
        return false;
    }
    static const std::set<std::string> supported_types = {
        "object", "array", "string", "boolean", "integer", "number", "null"
    };
    if (schema.contains("type") &&
        (!schema["type"].is_string() || !supported_types.count(schema["type"].get<std::string>()))) {
        if (error) *error = path + ".type is unsupported";
        return false;
    }
    if (schema.contains("properties")) {
        if (!schema["properties"].is_object()) {
            if (error) *error = path + ".properties must be an object";
            return false;
        }
        for (const auto& [name, child] : schema["properties"].items()) {
            if (name.empty() || !valid_schema(child, path + ".properties." + name, error)) return false;
        }
    }
    if (schema.contains("required")) {
        if (!schema["required"].is_array()) {
            if (error) *error = path + ".required must be an array";
            return false;
        }
        std::set<std::string> required_names;
        for (const auto& value : schema["required"]) {
            if (!value.is_string() || value.get<std::string>().empty() ||
                !required_names.insert(value.get<std::string>()).second) {
                if (error) *error = path + ".required contains an invalid or duplicate name";
                return false;
            }
            if (!schema.value("properties", json::object()).contains(value.get<std::string>())) {
                if (error) *error = path + ".required references an unknown property";
                return false;
            }
        }
    }
    if (schema.contains("items") && !valid_schema(schema["items"], path + ".items", error)) return false;
    if (schema.contains("enum") && !schema["enum"].is_array()) {
        if (error) *error = path + ".enum must be an array";
        return false;
    }
    if (schema.contains("additionalProperties") && !schema["additionalProperties"].is_boolean()) {
        if (error) *error = path + ".additionalProperties must be boolean";
        return false;
    }
    for (const auto* keyword : {"minLength", "maxLength", "minimum", "maximum"}) {
        if (schema.contains(keyword) && !schema[keyword].is_number()) {
            if (error) *error = path + "." + keyword + " must be numeric";
            return false;
        }
    }
    return true;
}

std::string json_type(const json& value) {
    if (value.is_object()) return "object";
    if (value.is_array()) return "array";
    if (value.is_string()) return "string";
    if (value.is_boolean()) return "boolean";
    if (value.is_number_integer() || value.is_number_unsigned()) return "integer";
    if (value.is_number()) return "number";
    if (value.is_null()) return "null";
    return "unknown";
}

json error_result(const std::string& code, const std::string& message) {
    return {{"isError", true},
            {"content", json::array({{{"type", "text"},
                                      {"text", json({{"ok", false},
                                                     {"error", {{"code", code}, {"message", message}}}}).dump()}}})},
            {"structuredContent", {{"ok", false},
                                    {"error", {{"code", code}, {"message", message}}}}}};
}

std::string sanitized_url(std::string value) {
    const auto fragment = value.find('#');
    if (fragment != std::string::npos) value.resize(fragment);
    const auto query = value.find('?');
    if (query != std::string::npos) value.resize(query);
    const auto scheme = value.find("://");
    if (scheme != std::string::npos) {
        const auto authority_end = value.find('/', scheme + 3);
        const auto at = value.find('@', scheme + 3);
        if (at != std::string::npos &&
            (authority_end == std::string::npos || at < authority_end)) {
            value.erase(scheme + 3, at - (scheme + 3) + 1);
        }
    }
    return value;
}

bool path_beneath(const std::filesystem::path& root, const std::filesystem::path& value) {
    auto root_part = root.begin();
    auto value_part = value.begin();
    for (; root_part != root.end(); ++root_part, ++value_part) {
        if (value_part == value.end() || *root_part != *value_part) return false;
    }
    return true;
}

json audit_targets(const RegisteredTool& tool, const json& arguments) {
    json targets = json::object();
    if (tool.plugin_id == "workspace_fs") {
        for (const auto* key : {"path", "source", "destination"}) {
            if (arguments.contains(key) && arguments[key].is_string()) targets[key] = arguments[key];
        }
    } else if (tool.id == "web_research.fetch_webpage" &&
               arguments.contains("url") && arguments["url"].is_string()) {
        targets["url"] = sanitized_url(arguments["url"].get<std::string>());
    }
    return targets;
}

void write_audit(const std::filesystem::path& sessions_root,
                 const ToolExecutionContext& context,
                 const RegisteredTool& tool,
                 const json& arguments,
                 std::int64_t duration_ms,
                 std::size_t result_bytes,
                 bool ok,
                 const std::string& error_code = {}) {
    try {
        json record = {{"timestamp_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::system_clock::now().time_since_epoch()).count()},
                       {"operation_id", context.operation_id},
                       {"mode_id", context.mode_id},
                       {"actor_kind", context.actor_kind},
                       {"actor_id", context.actor_id},
                       {"plugin_id", tool.plugin_id},
                       {"tool_id", tool.id},
                       {"targets", audit_targets(tool, arguments)},
                       {"duration_ms", duration_ms},
                       {"result_bytes", result_bytes},
                       {"ok", ok}};
        if (!error_code.empty()) record["error_code"] = error_code;
        const auto session_directory = sessions_root / context.session_id;
        const auto directory = sessions_root / context.session_id / "audit";
        std::lock_guard<std::mutex> lock(audit_mutex);
        std::error_code error;
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(session_directory, error)) ||
            std::filesystem::is_symlink(std::filesystem::symlink_status(directory, error))) return;
        const auto canonical_sessions = std::filesystem::weakly_canonical(sessions_root, error);
        const auto canonical_directory = std::filesystem::weakly_canonical(directory, error);
        if (error || !path_beneath(canonical_sessions, canonical_directory)) return;
        std::filesystem::create_directories(directory, error);
        if (error) return;
        std::ofstream output(directory / "tools.jsonl", std::ios::app);
        if (output) output << record.dump() << '\n';
    } catch (...) {
        // Audit I/O must not change a successful tool result.
    }
}

}  // namespace

bool ToolRegistry::Initialize(const std::filesystem::path& plugin_directory,
                              const std::filesystem::path& config_path,
                              std::string* error) {
    if (!LoadConfig(config_path, error)) return false;
    std::vector<std::string> plugin_ids(configured_plugins_.begin(), configured_plugins_.end());
    if (!loader_.LoadPlugins(plugin_directory.string(), plugin_ids)) {
        if (error) *error = "failed to load configured plugins from " + plugin_directory.string();
        return false;
    }
    return RegisterPlugins(error);
}

bool ToolRegistry::LoadConfig(const std::filesystem::path& path, std::string* error) {
    try {
        std::ifstream input(path);
        if (!input) throw std::runtime_error("cannot open tool config: " + path.string());
        json config;
        input >> config;
        if (config.value("schema_version", 0) != 1) {
            throw std::runtime_error("unsupported tool config schema_version");
        }
        const std::string permission = config.value("permission", "");
        if (permission != "allow") {
            throw std::runtime_error("only permission=allow is supported in this release");
        }
        if (!config.contains("modes") || !config["modes"].is_object()) {
            throw std::runtime_error("tool config must contain modes object");
        }
        for (const auto& [mode_id, mode] : config["modes"].items()) {
            if (!safe_token(mode_id) || !mode.is_object() ||
                !mode.contains("actors") || !mode["actors"].is_object()) {
                throw std::runtime_error("invalid mode entry: " + mode_id);
            }
            for (const auto& [actor, plugins] : mode["actors"].items()) {
                if (!safe_token(actor) || !plugins.is_array()) {
                    throw std::runtime_error("invalid actor entry: " + actor);
                }
                std::set<std::string> actor_plugins;
                for (const auto& plugin : plugins) {
                    if (!plugin.is_string() || !safe_token(plugin.get<std::string>())) {
                        throw std::runtime_error("invalid plugin id in actor mapping");
                    }
                    const auto id = plugin.get<std::string>();
                    if (!actor_plugins.insert(id).second) {
                        throw std::runtime_error("duplicate plugin id in actor mapping: " + id);
                    }
                    mode_actor_plugins_[mode_id][actor].insert(id);
                    configured_plugins_.insert(id);
                }
            }
        }
        if (configured_plugins_.empty()) throw std::runtime_error("no plugins configured");
        return true;
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return false;
    }
}

bool ToolRegistry::RegisterPlugins(std::string* error) {
    try {
        std::set<std::string> loaded_plugins;
        for (const auto& entry : loader_.GetPlugins()) {
            auto* plugin = entry.instance;
            if (!plugin || !plugin->GetApiVersion || !plugin->GetId ||
                !plugin->GetName || !plugin->GetVersion || !plugin->GetToolCount ||
                !plugin->GetTool || !plugin->HandleToolCall || !plugin->FreeResult ||
                plugin->GetApiVersion() != MCP_PLUGIN_API_VERSION) {
                throw std::runtime_error("plugin API version mismatch: " + entry.path);
            }
            const std::string plugin_id = plugin->GetId();
            const std::string plugin_name = plugin->GetName();
            const std::string plugin_version = plugin->GetVersion();
            static const std::regex version_pattern(R"(^[0-9]+\.[0-9]+\.[0-9]+(?:[-+][A-Za-z0-9.-]+)?$)");
            if (!safe_token(plugin_id) || plugin_name.empty() ||
                !std::regex_match(plugin_version, version_pattern) || plugin->GetToolCount() <= 0) {
                throw std::runtime_error("invalid plugin manifest: " + entry.path);
            }
            if (!configured_plugins_.count(plugin_id) || !loaded_plugins.insert(plugin_id).second) {
                throw std::runtime_error("unexpected or duplicate plugin: " + plugin_id);
            }
            for (int index = 0; index < plugin->GetToolCount(); ++index) {
                const auto* source = plugin->GetTool(index);
                if (!source || !source->id || !source->name || !source->inputSchema ||
                    !source->outputSchema) {
                    throw std::runtime_error("incomplete tool descriptor in " + plugin_id);
                }
                RegisteredTool tool;
                tool.id = source->id;
                tool.name = source->name;
                tool.plugin_id = plugin_id;
                tool.description = source->description ? source->description : "";
                tool.input_schema = json::parse(source->inputSchema);
                tool.output_schema = json::parse(source->outputSchema);
                tool.plugin = plugin;
                const std::string expected_name = plugin_id + "__" +
                    tool.id.substr(std::min(tool.id.size(), plugin_id.size() + 1));
                std::string schema_error;
                if (!safe_token(tool.id) || !safe_token(tool.name) || tool.description.empty() ||
                    tool.id.rfind(plugin_id + ".", 0) != 0 ||
                    tool.name != expected_name ||
                    !valid_schema(tool.input_schema, tool.id + ".inputSchema", &schema_error) ||
                    !valid_schema(tool.output_schema, tool.id + ".outputSchema", &schema_error)) {
                    throw std::runtime_error("invalid tool descriptor: " + tool.id);
                }
                if (!tools_by_id_.emplace(tool.id, tool).second ||
                    !id_by_name_.emplace(tool.name, tool.id).second) {
                    throw std::runtime_error("duplicate tool id or name: " + tool.id);
                }
            }
        }
        if (loaded_plugins != configured_plugins_) {
            throw std::runtime_error("one or more configured plugins were not loaded");
        }
        return true;
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return false;
    }
}

std::vector<RegisteredTool> ToolRegistry::ListTools(const ToolExecutionContext& context) const {
    std::vector<RegisteredTool> result;
    for (const auto& [id, tool] : tools_by_id_) {
        (void)id;
        if (IsAllowed(context, tool)) result.push_back(tool);
    }
    return result;
}

const RegisteredTool* ToolRegistry::Resolve(const std::string& id_or_name) const {
    auto direct = tools_by_id_.find(id_or_name);
    if (direct != tools_by_id_.end()) return &direct->second;
    const auto alias = id_by_name_.find(id_or_name);
    if (alias == id_by_name_.end()) return nullptr;
    return &tools_by_id_.at(alias->second);
}

bool ToolRegistry::IsAllowed(const ToolExecutionContext& context,
                             const RegisteredTool& tool) const {
    const auto mode = mode_actor_plugins_.find(context.mode_id);
    if (mode == mode_actor_plugins_.end()) return false;
    const auto actor = mode->second.find(context.actor_kind);
    if (actor == mode->second.end()) return false;
    return permission_evaluator_.Evaluate(context, tool) == PermissionDecision::Allow &&
           actor->second.count(tool.plugin_id) > 0;
}

bool ToolGateway::ParseContext(const json& params,
                               ToolExecutionContext* context,
                               std::string* error) {
    if (!params.is_object() || !params.contains("context") || !params["context"].is_object()) {
        if (error) *error = "context object is required";
        return false;
    }
    const auto& value = params["context"];
    for (const auto* field : {"session_id", "operation_id", "mode_id", "actor_kind", "actor_id"}) {
        if (!value.contains(field) || !value[field].is_string()) {
            if (error) *error = std::string("context.") + field + " must be a string";
            return false;
        }
    }
    if (value.contains("trusted_data") && !value["trusted_data"].is_object()) {
        if (error) *error = "context.trusted_data must be an object";
        return false;
    }
    context->session_id = value["session_id"].get<std::string>();
    context->operation_id = value["operation_id"].get<std::string>();
    context->mode_id = value["mode_id"].get<std::string>();
    context->actor_kind = value["actor_kind"].get<std::string>();
    context->actor_id = value["actor_id"].get<std::string>();
    context->trusted_data = value.value("trusted_data", json::object());
    static const std::regex session_pattern("^[a-f0-9]{32}$");
    if (!std::regex_match(context->session_id, session_pattern) ||
        !safe_token(context->operation_id) || !safe_token(context->mode_id) ||
        !safe_token(context->actor_kind) || !safe_token(context->actor_id)) {
        if (error) *error = "invalid execution context";
        return false;
    }
    return true;
}

json ToolGateway::List(const ToolExecutionContext& context) const {
    json tools = json::array();
    for (const auto& tool : registry_.ListTools(context)) {
        tools.push_back({{"name", tool.name}, {"toolId", tool.id},
                         {"pluginId", tool.plugin_id}, {"description", tool.description},
                         {"inputSchema", tool.input_schema},
                         {"outputSchema", tool.output_schema}});
    }
    return tools;
}

json ToolGateway::Call(const ToolExecutionContext& context,
                       const std::string& tool_name,
                       const json& arguments) const {
    const auto* tool = registry_.Resolve(tool_name);
    if (!tool || !registry_.IsAllowed(context, *tool)) {
        return error_result("TOOL_NOT_GRANTED", "tool is not granted to this mode and actor");
    }
    std::string validation_error;
    if (!ValidateArguments(tool->input_schema, arguments, &validation_error)) {
        return error_result("INVALID_ARGUMENTS", validation_error);
    }
    const auto workspace = registry_.sessions_root() / context.session_id / "workspace";
    std::error_code ec;
    const auto session_directory = registry_.sessions_root() / context.session_id;
    const auto session_status = std::filesystem::symlink_status(session_directory, ec);
    const auto workspace_status = std::filesystem::symlink_status(workspace, ec);
    const auto canonical_sessions = std::filesystem::weakly_canonical(registry_.sessions_root(), ec);
    const auto canonical_workspace = std::filesystem::weakly_canonical(workspace, ec);
    if (ec || std::filesystem::is_symlink(session_status) ||
        std::filesystem::is_symlink(workspace_status) ||
        !std::filesystem::exists(workspace_status) ||
        !std::filesystem::is_directory(workspace_status) ||
        !path_beneath(canonical_sessions, canonical_workspace)) {
        return error_result("UNKNOWN_SESSION", "session workspace does not exist");
    }
    json plugin_context = {{"session_id", context.session_id},
                           {"operation_id", context.operation_id},
                           {"mode_id", context.mode_id},
                           {"actor_kind", context.actor_kind},
                           {"actor_id", context.actor_id},
                           {"workspace_root", workspace.string()},
                           {"trusted_data", context.trusted_data}};
    const auto started = std::chrono::steady_clock::now();
    char* raw = tool->plugin->HandleToolCall(tool->id.c_str(), arguments.dump().c_str(),
                                             plugin_context.dump().c_str());
    const auto elapsed_ms = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
    };
    if (!raw) {
        write_audit(registry_.sessions_root(), context, *tool, arguments,
                    elapsed_ms(), 0, false, "PLUGIN_ERROR");
        return error_result("PLUGIN_ERROR", "plugin returned no result");
    }
    const std::string raw_result(raw);
    tool->plugin->FreeResult(raw);
    if (raw_result.size() > kMaxPluginResultBytes) {
        write_audit(registry_.sessions_root(), context, *tool, arguments,
                    elapsed_ms(), raw_result.size(), false, "PLUGIN_OUTPUT_TOO_LARGE");
        return error_result("PLUGIN_OUTPUT_TOO_LARGE", "plugin result exceeds 2 MiB limit");
    }
    json result;
    try {
        result = json::parse(raw_result);
    } catch (...) {
        write_audit(registry_.sessions_root(), context, *tool, arguments,
                    elapsed_ms(), raw_result.size(), false, "PLUGIN_ERROR");
        return error_result("PLUGIN_ERROR", "plugin returned invalid JSON");
    }
    std::string output_error;
    if (!ValidateArguments(tool->output_schema, result, &output_error)) {
        write_audit(registry_.sessions_root(), context, *tool, arguments,
                    elapsed_ms(), raw_result.size(), false, "INVALID_PLUGIN_OUTPUT");
        return error_result("INVALID_PLUGIN_OUTPUT", output_error);
    }
    const bool ok = result.value("ok", false);
    const std::string plugin_error = ok ? "" : result.value("error", json::object()).value("code", "PLUGIN_ERROR");
    write_audit(registry_.sessions_root(), context, *tool, arguments,
                elapsed_ms(), raw_result.size(), ok, plugin_error);
    return {{"isError", !ok},
            {"content", json::array({{{"type", "text"}, {"text", result.dump()}}})},
            {"structuredContent", result}};
}

bool ToolGateway::ValidateArguments(const json& schema,
                                    const json& arguments,
                                    std::string* error) {
    return ValidateValue(schema, arguments, "$", error);
}

bool ToolGateway::ValidateValue(const json& schema,
                                const json& value,
                                const std::string& path,
                                std::string* error) {
    if (!schema.is_object()) return true;
    if (schema.contains("type") && schema["type"].is_string()) {
        const auto expected = schema["type"].get<std::string>();
        const auto actual = json_type(value);
        if (expected != actual && !(expected == "number" && actual == "integer")) {
            if (error) *error = path + " must be " + expected + ", got " + actual;
            return false;
        }
    }
    if (schema.contains("enum") && schema["enum"].is_array() &&
        std::find(schema["enum"].begin(), schema["enum"].end(), value) == schema["enum"].end()) {
        if (error) *error = path + " is not an allowed value";
        return false;
    }
    if (value.is_object()) {
        if (schema.contains("required") && schema["required"].is_array()) {
            for (const auto& required : schema["required"]) {
                if (required.is_string() && !value.contains(required.get<std::string>())) {
                    if (error) *error = path + "." + required.get<std::string>() + " is required";
                    return false;
                }
            }
        }
        const auto properties = schema.value("properties", json::object());
        for (const auto& [key, child] : value.items()) {
            if (properties.contains(key)) {
                if (!ValidateValue(properties[key], child, path + "." + key, error)) return false;
            } else if (schema.value("additionalProperties", true) == false) {
                if (error) *error = path + "." + key + " is not allowed";
                return false;
            }
        }
    }
    if (value.is_array() && schema.contains("items")) {
        for (std::size_t index = 0; index < value.size(); ++index) {
            if (!ValidateValue(schema["items"], value[index],
                               path + "[" + std::to_string(index) + "]", error)) return false;
        }
    }
    if (value.is_string()) {
        if (schema.contains("minLength") && value.get_ref<const std::string&>().size() < schema["minLength"].get<std::size_t>()) {
            if (error) *error = path + " is too short";
            return false;
        }
        if (schema.contains("maxLength") && value.get_ref<const std::string&>().size() > schema["maxLength"].get<std::size_t>()) {
            if (error) *error = path + " is too long";
            return false;
        }
    }
    if (value.is_number()) {
        const double number = value.get<double>();
        if (schema.contains("minimum") && number < schema["minimum"].get<double>()) {
            if (error) *error = path + " is below minimum";
            return false;
        }
        if (schema.contains("maximum") && number > schema["maximum"].get<double>()) {
            if (error) *error = path + " is above maximum";
            return false;
        }
    }
    return true;
}

}  // namespace vx::mcp
