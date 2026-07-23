#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace ai_orchestrator::dag {

using json = nlohmann::json;

struct AgentDefinition {
    std::string id;
    std::string name;
    std::string role;
    std::string icon = "A";
    std::vector<std::string> private_materials;

    json public_json() const;
};

struct PlanItem {
    std::string agent_id;
    std::string subtask;
    std::vector<std::string> depends_on;

    json to_json() const;
};

struct TraceEvent {
    std::string type;
    std::string title;
    json payload = json::object();
    std::int64_t seq = 0;
    std::int64_t timestamp_ms = 0;

    json to_json() const;
};

struct SkillSpec {
    std::string id;
    std::string name;
    std::string description;
    std::string instructions;
    std::vector<std::string> keywords;
    bool always_on = false;
};

struct SkillActivation {
    SkillSpec skill;
    double score = 0.0;
    std::string reason;

    json public_json() const;
};

struct ToolDefinition {
    std::string name;
    std::string description;
    std::string parameters_json;
};

struct ToolResult {
    bool success = false;
    std::string content;
    std::string error;
};

using ToolExecutor = std::function<ToolResult(const std::string&, const std::string&)>;

class ToolRegistry {
public:
    void add(ToolDefinition definition, ToolExecutor executor);
    std::vector<ToolDefinition> definitions() const;
    ToolResult execute(const std::string& name, const std::string& arguments_json) const;
    bool empty() const;

private:
    struct Entry {
        ToolDefinition definition;
        ToolExecutor executor;
    };
    std::map<std::string, Entry> entries_;
};

struct ChatMessage {
    std::string role;
    std::string content;
};

class ChatModel {
public:
    virtual ~ChatModel() = default;
    virtual std::string complete(const std::vector<ChatMessage>& messages,
                                 double temperature) = 0;
};

class QwenChatModel final : public ChatModel {
public:
    explicit QwenChatModel(std::string api_key,
                           std::string model = "qwen-plus",
                           std::string endpoint =
                               "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions");
    ~QwenChatModel() override;

    std::string complete(const std::vector<ChatMessage>& messages,
                         double temperature) override;

private:
    std::string api_key_;
    std::string model_;
    std::string endpoint_;
};

class LambdaChatModel final : public ChatModel {
public:
    using Handler = std::function<std::string(const std::vector<ChatMessage>&, double)>;

    explicit LambdaChatModel(Handler handler);
    std::string complete(const std::vector<ChatMessage>& messages,
                         double temperature) override;

private:
    Handler handler_;
};

std::vector<AgentDefinition> default_agents();
std::vector<SkillSpec> default_skills();

std::string build_agent_design_prompt(
    int max_agents,
    const std::vector<std::string>& reference_materials = {});
std::string build_planner_prompt(
    const std::vector<AgentDefinition>& agents,
    const std::vector<std::string>& reference_materials = {});

std::vector<AgentDefinition> parse_agent_design(
    const std::string& raw,
    const std::string& user_input,
    int max_agents);
std::vector<PlanItem> parse_plan(
    const std::string& raw,
    const std::vector<AgentDefinition>& agents,
    const std::string& user_input);

struct RunOptions {
    std::string user_input;
    std::vector<AgentDefinition> agents;
    std::vector<std::string> reference_materials;
    std::map<std::string, std::vector<std::string>> agent_reference_materials;
    bool auto_agents = false;
    int max_dynamic_agents = 6;
    int max_agent_rounds = 5;
    // Disabled by default while preserving the switch for a future bounded
    // context mode. ReAct keeps the complete extracted memory between rounds.
    bool enable_memory_compression = false;
};

class DagRuntime {
public:
    explicit DagRuntime(std::shared_ptr<ChatModel> model,
                        std::filesystem::path log_directory = {});

    std::vector<AgentDefinition> agents() const;
    void set_default_agents(std::vector<AgentDefinition> agents);

    // Writes the exact chat messages submitted to the model and its raw
    // response. The log filename is reduced to a safe basename internally.
    // This is public so non-run model calls (for example Agent Designer
    // drafts from the Web API) use the same logging path and format.
    void log_chat_exchange(const std::string& log_file,
                           const std::string& phase,
                           const std::string& run_id,
                           const std::string& agent_id,
                           int round,
                           double temperature,
                           const std::vector<ChatMessage>& messages,
                           const std::string& response = {},
                           const std::string& error = {}) const;

    std::string create_run(RunOptions options);
    json snapshot(const std::string& run_id) const;
    bool retry_agent(const std::string& run_id,
                     const std::string& agent_id,
                     const std::string& edited_prior,
                     const std::string& user_feedback);

private:
    struct RunState;

    void execute_run(const std::shared_ptr<RunState>& state);
    void execute_retry(const std::shared_ptr<RunState>& state,
                       const std::string& agent_id);
    bool execute_node(const std::shared_ptr<RunState>& state,
                      const PlanItem& node,
                      std::string* error);
    std::vector<SkillActivation> select_skills(
        const std::string& user_input,
        const PlanItem& node,
        const AgentDefinition& agent) const;
    std::string run_react(const std::shared_ptr<RunState>& state,
                          const PlanItem& node,
                          const AgentDefinition& agent,
                          const std::vector<SkillActivation>& skills);
    void emit(const std::shared_ptr<RunState>& state,
              const std::string& type,
              const std::string& title,
              json payload = json::object()) const;

    std::shared_ptr<ChatModel> model_;
    std::vector<AgentDefinition> default_agents_;
    std::vector<SkillSpec> skills_;
    ToolRegistry tools_;
    std::filesystem::path log_directory_;
    mutable std::mutex log_mutex_;
    mutable std::mutex runs_mutex_;
    std::map<std::string, std::shared_ptr<RunState>> runs_;
};

}  // namespace ai_orchestrator::dag
