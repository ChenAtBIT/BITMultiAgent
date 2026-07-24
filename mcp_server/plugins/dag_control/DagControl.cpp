#include "PluginAPI.h"
#include "PluginSupport.h"

#include <algorithm>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

using vx::plugin::json;

namespace {

std::mutex state_mutex;
std::unordered_map<std::string, json> drafts;
constexpr const char* kOutput = R"({"type":"object","properties":{"ok":{"type":"boolean"},"result":{"type":"object"},"error":{"type":"object","properties":{"code":{"type":"string"},"message":{"type":"string"}},"required":["code","message"],"additionalProperties":false}},"required":["ok"],"additionalProperties":false})";

PluginTool tools[] = {
    {"dag_control.list_plan", "dag_control__list_plan", "List the current DAG plan draft.", R"({"type":"object","properties":{},"additionalProperties":false})", kOutput},
    {"dag_control.add_node", "dag_control__add_node", "Add a node to the DAG plan draft.", R"({"type":"object","properties":{"agent_id":{"type":"string","description":"Agent id from the trusted available Agent list to assign this DAG node to.","minLength":1},"subtask":{"type":"string","description":"Specific executable subtask that this Agent should complete.","minLength":1,"maxLength":4000},"depends_on":{"type":"array","description":"Agent ids for upstream nodes that must finish before this node can run; use [] when there are no dependencies.","items":{"type":"string","description":"Agent id of an upstream dependency node."}}},"required":["agent_id","subtask","depends_on"],"additionalProperties":false})", kOutput},
    {"dag_control.update_node", "dag_control__update_node", "Update a node in the DAG plan draft.", R"({"type":"object","properties":{"agent_id":{"type":"string","description":"Agent id of the existing DAG node to update.","minLength":1},"subtask":{"type":"string","description":"Replacement executable subtask for this DAG node.","minLength":1,"maxLength":4000},"depends_on":{"type":"array","description":"Replacement upstream dependency Agent ids; use [] when there are no dependencies.","items":{"type":"string","description":"Agent id of an upstream dependency node."}}},"required":["agent_id"],"additionalProperties":false})", kOutput},
    {"dag_control.remove_node", "dag_control__remove_node", "Remove a node from the DAG plan draft.", R"({"type":"object","properties":{"agent_id":{"type":"string","description":"Agent id of the DAG node to remove.","minLength":1}},"required":["agent_id"],"additionalProperties":false})", kOutput},
    {"dag_control.validate_plan", "dag_control__validate_plan", "Validate the current DAG without committing it.", R"({"type":"object","properties":{},"additionalProperties":false})", kOutput},
    {"dag_control.commit_plan", "dag_control__commit_plan", "Validate and commit the current DAG plan.", R"({"type":"object","properties":{},"additionalProperties":false})", kOutput}
};

json validate(const json& plan, const json& context) {
    if (!plan.is_array() || plan.empty()) return vx::plugin::failure("EMPTY_PLAN", "DAG plan cannot be empty");
    std::set<std::string> allowed;
    const auto trusted = context.value("trusted_data", json::object());
    if (trusted.contains("agent_ids") && trusted["agent_ids"].is_array()) {
        for (const auto& value : trusted["agent_ids"]) if (value.is_string()) allowed.insert(value.get<std::string>());
    }
    std::set<std::string> ids;
    std::map<std::string, std::vector<std::string>> dependencies;
    for (const auto& node : plan) {
        const std::string id = node.value("agent_id", "");
        if (!allowed.count(id)) return vx::plugin::failure("UNKNOWN_AGENT", "plan uses an Agent outside the trusted team: " + id);
        if (!ids.insert(id).second) return vx::plugin::failure("DUPLICATE_NODE", "duplicate DAG node: " + id);
        if (node.value("subtask", "").empty()) return vx::plugin::failure("INVALID_NODE", "subtask cannot be empty");
        dependencies[id] = node.value("depends_on", std::vector<std::string>{});
    }
    for (const auto& [id, deps] : dependencies) {
        std::set<std::string> unique;
        for (const auto& dep : deps) {
            if (dep == id) return vx::plugin::failure("SELF_DEPENDENCY", "node cannot depend on itself: " + id);
            if (!ids.count(dep)) return vx::plugin::failure("UNKNOWN_DEPENDENCY", "dependency is not in the plan: " + dep);
            if (!unique.insert(dep).second) return vx::plugin::failure("DUPLICATE_DEPENDENCY", "duplicate dependency: " + dep);
        }
    }
    std::set<std::string> visiting, visited;
    std::function<bool(const std::string&)> visit = [&](const std::string& id) {
        if (visiting.count(id)) return true;
        if (visited.count(id)) return false;
        visiting.insert(id);
        for (const auto& dep : dependencies[id]) if (visit(dep)) return true;
        visiting.erase(id);
        visited.insert(id);
        return false;
    };
    for (const auto& id : ids) if (visit(id)) return vx::plugin::failure("CYCLIC_PLAN", "DAG contains a cycle");
    return vx::plugin::success({{"valid", true}, {"plan", plan}});
}

char* handle(const char* tool_id, const char* raw_arguments, const char* raw_context) {
    try {
        const json arguments = json::parse(raw_arguments ? raw_arguments : "{}");
        const json context = json::parse(raw_context ? raw_context : "{}");
        const std::string key = vx::plugin::state_key(context);
        const std::string id = tool_id ? tool_id : "";
        std::lock_guard<std::mutex> lock(state_mutex);
        auto [it, inserted] = drafts.emplace(key, json::array());
        if (inserted) it->second = json::array();
        auto& plan = it->second;
        if (id == "dag_control.list_plan") return vx::plugin::copy_result(vx::plugin::success({{"plan", plan}}));
        if (id == "dag_control.add_node") {
            const std::string agent_id = arguments.value("agent_id", "");
            if (std::any_of(plan.begin(), plan.end(), [&](const json& node) { return node.value("agent_id", "") == agent_id; })) {
                return vx::plugin::copy_result(vx::plugin::failure("DUPLICATE_NODE", "node already exists"));
            }
            plan.push_back(arguments);
            return vx::plugin::copy_result(vx::plugin::success({{"plan", plan}}));
        }
        if (id == "dag_control.update_node") {
            const std::string agent_id = arguments.value("agent_id", "");
            for (auto& node : plan) {
                if (node.value("agent_id", "") != agent_id) continue;
                if (arguments.contains("subtask")) node["subtask"] = arguments["subtask"];
                if (arguments.contains("depends_on")) node["depends_on"] = arguments["depends_on"];
                return vx::plugin::copy_result(vx::plugin::success({{"plan", plan}}));
            }
            return vx::plugin::copy_result(vx::plugin::failure("UNKNOWN_NODE", "node does not exist"));
        }
        if (id == "dag_control.remove_node") {
            const std::string agent_id = arguments.value("agent_id", "");
            const auto old_size = plan.size();
            plan.erase(std::remove_if(plan.begin(), plan.end(), [&](const json& node) { return node.value("agent_id", "") == agent_id; }), plan.end());
            if (old_size == plan.size()) return vx::plugin::copy_result(vx::plugin::failure("UNKNOWN_NODE", "node does not exist"));
            for (auto& node : plan) {
                auto deps = node.value("depends_on", std::vector<std::string>{});
                deps.erase(std::remove(deps.begin(), deps.end(), agent_id), deps.end());
                node["depends_on"] = deps;
            }
            return vx::plugin::copy_result(vx::plugin::success({{"plan", plan}}));
        }
        if (id == "dag_control.validate_plan" || id == "dag_control.commit_plan") {
            json result = validate(plan, context);
            if (result.value("ok", false) && id == "dag_control.commit_plan") {
                result["result"]["committed"] = true;
                drafts.erase(key);
            }
            return vx::plugin::copy_result(result);
        }
        return vx::plugin::copy_result(vx::plugin::failure("UNKNOWN_TOOL", "unknown dag_control tool"));
    } catch (const std::exception& exception) {
        return vx::plugin::copy_result(vx::plugin::failure("PLUGIN_ERROR", exception.what()));
    }
}

int api_version() { return MCP_PLUGIN_API_VERSION; }
const char* plugin_id() { return "dag_control"; }
const char* plugin_name() { return "DAG Control"; }
const char* plugin_version() { return "1.0.0"; }
int initialize() { return 1; }
void shutdown() { std::lock_guard<std::mutex> lock(state_mutex); drafts.clear(); }
int tool_count() { return static_cast<int>(sizeof(tools) / sizeof(tools[0])); }
const PluginTool* get_tool(int index) { return index >= 0 && index < tool_count() ? &tools[index] : nullptr; }

}  // namespace

extern "C" PLUGIN_API PluginAPI* CreatePlugin() {
    return new PluginAPI{api_version, plugin_id, plugin_name, plugin_version, initialize,
                         shutdown, tool_count, get_tool, handle, vx::plugin::free_result};
}
extern "C" PLUGIN_API void DestroyPlugin(PluginAPI* api) { delete api; }
