#include "PluginAPI.h"
#include "PluginSupport.h"

#include <algorithm>
#include <mutex>
#include <regex>
#include <set>
#include <string>
#include <unordered_map>

using vx::plugin::json;

namespace {

std::mutex state_mutex;
std::unordered_map<std::string, json> drafts;

constexpr const char* kOutput = R"({"type":"object","properties":{"ok":{"type":"boolean"},"result":{"type":"object"},"error":{"type":"object","properties":{"code":{"type":"string"},"message":{"type":"string"}},"required":["code","message"],"additionalProperties":false}},"required":["ok"],"additionalProperties":false})";

PluginTool tools[] = {
    {"team_design.list_team", "team_design__list_team", "List the current Agent team draft.",
     R"({"type":"object","properties":{},"additionalProperties":false})", kOutput},
    {"team_design.add_agent", "team_design__add_agent", "Add one Agent to the team draft.",
     R"({"type":"object","properties":{"id":{"type":"string","description":"Unique lowercase snake_case Agent id used internally by the DAG planner.","minLength":1,"maxLength":64},"name":{"type":"string","description":"Short display name for the Agent role.","minLength":1,"maxLength":120},"role":{"type":"string","description":"Concise responsibility description explaining what this Agent should handle for the user task.","minLength":1,"maxLength":1200},"icon":{"type":"string","description":"One or two uppercase letters used as a compact visual abbreviation.","maxLength":2}},"required":["id","name","role"],"additionalProperties":false})", kOutput},
    {"team_design.update_agent", "team_design__update_agent", "Update an existing Agent in the team draft.",
     R"({"type":"object","properties":{"id":{"type":"string","description":"Unique id of the existing Agent to update.","minLength":1},"name":{"type":"string","description":"Replacement short display name for the Agent role.","minLength":1,"maxLength":120},"role":{"type":"string","description":"Replacement responsibility description for the Agent.","minLength":1,"maxLength":1200},"icon":{"type":"string","description":"Replacement one or two uppercase letter visual abbreviation.","maxLength":2}},"required":["id"],"additionalProperties":false})", kOutput},
    {"team_design.remove_agent", "team_design__remove_agent", "Remove an Agent from the team draft.",
     R"({"type":"object","properties":{"id":{"type":"string","description":"Unique id of the Agent to remove from the draft.","minLength":1}},"required":["id"],"additionalProperties":false})", kOutput},
    {"team_design.commit_team", "team_design__commit_team", "Validate and commit the Agent team draft.",
     R"({"type":"object","properties":{},"additionalProperties":false})", kOutput}
};

bool valid_id(const std::string& value) {
    static const std::regex pattern("^[a-z][a-z0-9_]{0,63}$");
    return std::regex_match(value, pattern);
}

json& draft_for(const std::string& key) {
    auto [it, inserted] = drafts.emplace(key, json::array());
    if (inserted) it->second = json::array();
    return it->second;
}

char* handle(const char* tool_id, const char* raw_arguments, const char* raw_context) {
    try {
        const json arguments = json::parse(raw_arguments ? raw_arguments : "{}");
        const json context = json::parse(raw_context ? raw_context : "{}");
        const std::string key = vx::plugin::state_key(context);
        const std::string id = tool_id ? tool_id : "";
        std::lock_guard<std::mutex> lock(state_mutex);
        auto& team = draft_for(key);

        if (id == "team_design.list_team") return vx::plugin::copy_result(vx::plugin::success({{"agents", team}}));
        if (id == "team_design.add_agent") {
            const std::string agent_id = arguments.value("id", "");
            if (!valid_id(agent_id)) return vx::plugin::copy_result(vx::plugin::failure("INVALID_AGENT_ID", "agent id must be lowercase snake_case"));
            if (std::any_of(team.begin(), team.end(), [&](const json& item) { return item.value("id", "") == agent_id; })) {
                return vx::plugin::copy_result(vx::plugin::failure("DUPLICATE_AGENT", "agent id already exists"));
            }
            team.push_back({{"id", agent_id}, {"name", arguments.value("name", "")},
                            {"role", arguments.value("role", "")},
                            {"icon", arguments.value("icon", "A")}});
            return vx::plugin::copy_result(vx::plugin::success({{"agents", team}}));
        }
        if (id == "team_design.update_agent") {
            const std::string agent_id = arguments.value("id", "");
            for (auto& agent : team) {
                if (agent.value("id", "") != agent_id) continue;
                for (const auto* field : {"name", "role", "icon"}) {
                    if (arguments.contains(field)) agent[field] = arguments[field];
                }
                return vx::plugin::copy_result(vx::plugin::success({{"agents", team}}));
            }
            return vx::plugin::copy_result(vx::plugin::failure("UNKNOWN_AGENT", "agent does not exist"));
        }
        if (id == "team_design.remove_agent") {
            const std::string agent_id = arguments.value("id", "");
            const auto old_size = team.size();
            team.erase(std::remove_if(team.begin(), team.end(), [&](const json& item) {
                return item.value("id", "") == agent_id;
            }), team.end());
            if (old_size == team.size()) return vx::plugin::copy_result(vx::plugin::failure("UNKNOWN_AGENT", "agent does not exist"));
            return vx::plugin::copy_result(vx::plugin::success({{"agents", team}}));
        }
        if (id == "team_design.commit_team") {
            const int maximum = context.value("trusted_data", json::object()).value("max_agents", 6);
            if (team.size() < 3 || static_cast<int>(team.size()) > maximum) {
                return vx::plugin::copy_result(vx::plugin::failure("INVALID_TEAM_SIZE", "team must contain between 3 and max_agents Agents"));
            }
            for (const auto& agent : team) {
                if (!valid_id(agent.value("id", "")) || agent.value("name", "").empty() || agent.value("role", "").empty()) {
                    return vx::plugin::copy_result(vx::plugin::failure("INVALID_AGENT", "all Agents require valid id, name, and role"));
                }
            }
            const json committed = team;
            drafts.erase(key);
            return vx::plugin::copy_result(vx::plugin::success({{"committed", true}, {"agents", committed}}));
        }
        return vx::plugin::copy_result(vx::plugin::failure("UNKNOWN_TOOL", "unknown team_design tool"));
    } catch (const std::exception& exception) {
        return vx::plugin::copy_result(vx::plugin::failure("PLUGIN_ERROR", exception.what()));
    }
}

int api_version() { return MCP_PLUGIN_API_VERSION; }
const char* plugin_id() { return "team_design"; }
const char* plugin_name() { return "Agent Team Design"; }
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
