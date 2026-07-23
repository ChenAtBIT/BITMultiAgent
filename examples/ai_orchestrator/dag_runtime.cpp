#include "ai_orchestrator/dag_runtime.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <random>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace ai_orchestrator::dag {
namespace {

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string trim(std::string value) {
    auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string json_safe_text(const std::string& value);

std::string limit_text(const std::string& value, std::size_t limit) {
    // Context compression is intentionally disabled for now. Keep the
    // parameter in the helper signature so existing call sites remain
    // explicit about where a future policy can be reintroduced. Normalize
    // malformed bytes only; this is transport safety, not length compression.
    (void)limit;
    return json_safe_text(value);
}

std::string join(const std::vector<std::string>& values, const std::string& separator) {
    std::ostringstream out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out << separator;
        out << values[i];
    }
    return out.str();
}

std::string render_materials(const std::vector<std::string>& values) {
    if (values.empty()) return "No supplemental reference materials were supplied.";
    std::ostringstream out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        out << "[Material " << (i + 1) << "]\n" << values[i] << "\n";
    }
    return out.str();
}

std::string extract_array(const std::string& raw) {
    const auto begin = raw.find('[');
    const auto end = raw.rfind(']');
    if (begin == std::string::npos || end <= begin) return {};
    return raw.substr(begin, end - begin + 1);
}

std::string extract_object(const std::string& raw) {
    const auto begin = raw.find('{');
    const auto end = raw.rfind('}');
    if (begin == std::string::npos || end <= begin) return {};
    return raw.substr(begin, end - begin + 1);
}

std::string safe_id(std::string value, const std::string& fallback) {
    value = lower(trim(value));
    std::string result;
    for (char ch : value) {
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_') result += ch;
        else if (ch == '-' || std::isspace(static_cast<unsigned char>(ch))) result += '_';
    }
    while (result.find("__") != std::string::npos) {
        result.replace(result.find("__"), 2, "_");
    }
    if (result.empty()) result = fallback;
    return result;
}

std::string safe_log_file_name(std::string value) {
    value = std::filesystem::path(value).filename().string();
    std::string result;
    for (const auto ch : value) {
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-' || ch == '.') {
            result += ch;
        } else {
            result += '_';
        }
    }
    if (result.empty() || result == "." || result == "..") result = "runtime.log";
    if (result.find('.') == std::string::npos) result += ".log";
    return result;
}

// nlohmann::json requires UTF-8 strings. User-provided materials and model
// gateways can occasionally contain a stray non-UTF-8 byte. Preserve those
// bytes in the diagnostic log as an ASCII \\xHH escape instead of allowing a
// logging exception to fail the Agent node.
std::string json_safe_text(const std::string& value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size();) {
        const auto first = static_cast<unsigned char>(value[index]);
        std::size_t length = 0;
        bool valid = true;
        if (first <= 0x7F) {
            length = 1;
        } else if (first >= 0xC2 && first <= 0xDF) {
            length = 2;
        } else if (first == 0xE0) {
            length = 3;
            valid = index + 2 < value.size() &&
                    static_cast<unsigned char>(value[index + 1]) >= 0xA0 &&
                    static_cast<unsigned char>(value[index + 1]) <= 0xBF;
        } else if (first >= 0xE1 && first <= 0xEC) {
            length = 3;
        } else if (first == 0xED) {
            length = 3;
            valid = index + 1 < value.size() &&
                    static_cast<unsigned char>(value[index + 1]) >= 0x80 &&
                    static_cast<unsigned char>(value[index + 1]) <= 0x9F;
        } else if (first >= 0xEE && first <= 0xEF) {
            length = 3;
        } else if (first == 0xF0) {
            length = 4;
            valid = index + 2 < value.size() &&
                    static_cast<unsigned char>(value[index + 1]) >= 0x90 &&
                    static_cast<unsigned char>(value[index + 1]) <= 0xBF;
        } else if (first >= 0xF1 && first <= 0xF3) {
            length = 4;
        } else if (first == 0xF4) {
            length = 4;
            valid = index + 2 < value.size() &&
                    static_cast<unsigned char>(value[index + 1]) >= 0x80 &&
                    static_cast<unsigned char>(value[index + 1]) <= 0x8F;
        } else {
            valid = false;
            length = 1;
        }

        if (valid && index + length <= value.size()) {
            for (std::size_t offset = 1; offset < length; ++offset) {
                const auto continuation = static_cast<unsigned char>(value[index + offset]);
                if (continuation < 0x80 || continuation > 0xBF) {
                    valid = false;
                    break;
                }
            }
        } else {
            valid = false;
        }

        if (valid) {
            result.append(value, index, length);
            index += length;
            continue;
        }

        result += "\\x";
        result += hex[(first >> 4) & 0x0F];
        result += hex[first & 0x0F];
        ++index;
    }
    return result;
}

std::string output_text(const json& message) {
    if (!message.contains("content") || message["content"].is_null()) return {};
    if (message["content"].is_string()) return message["content"].get<std::string>();
    if (message["content"].is_array()) {
        std::string result;
        for (const auto& item : message["content"]) {
            if (item.is_string()) result += item.get<std::string>();
            else if (item.is_object() && item.value("type", "") == "text") {
                result += item.value("text", "");
            }
        }
        return result;
    }
    return message["content"].dump();
}

size_t curl_write(void* contents, size_t size, size_t count, void* user_data) {
    auto* output = static_cast<std::string*>(user_data);
    output->append(static_cast<const char*>(contents), size * count);
    return size * count;
}

std::vector<std::string> tokenize(const std::string& text) {
    std::vector<std::string> result;
    std::string current;
    for (unsigned char ch : text) {
        if (std::isalnum(ch) || ch >= 128 || ch == '_') {
            current += static_cast<char>(std::tolower(ch));
        } else if (!current.empty()) {
            result.push_back(current);
            current.clear();
        }
    }
    if (!current.empty()) result.push_back(current);
    return result;
}

std::set<std::string> token_set(const std::string& text) {
    static const std::set<std::string> stopwords = {
        "a", "an", "and", "for", "in", "of", "or", "the", "to", "with",
        "agent", "content", "draft", "task", "write", "writing"
    };
    std::set<std::string> result;
    for (const auto& token : tokenize(text)) {
        if (!stopwords.count(token)) result.insert(token);
    }
    return result;
}

std::string compact_dependencies(const std::map<std::string, std::string>& outputs) {
    if (outputs.empty()) return "No upstream outputs were required for this step.";
    std::ostringstream out;
    for (const auto& [id, value] : outputs) {
        out << "- " << id << ": " << limit_text(value, 700) << "\n";
    }
    return out.str();
}

std::string compact_skills(const std::vector<SkillActivation>& skills) {
    if (skills.empty()) return "No specialized skills were activated for this step.";
    std::ostringstream out;
    for (const auto& activation : skills) {
        out << "- " << activation.skill.name << ": " << activation.skill.instructions << "\n";
    }
    return out.str();
}

json safe_json_parse(const std::string& raw) {
    try { return json::parse(raw); } catch (...) { return json(); }
}

struct ParsedReact {
    std::string action;
    std::string observation;
    std::string answer;
};

ParsedReact parse_react(const std::string& raw, bool force_final) {
    const auto parsed = safe_json_parse(extract_object(raw));
    if (!parsed.is_object()) {
        return {force_final ? "final" : "reason", limit_text(trim(raw), 1400),
                force_final ? trim(raw) : ""};
    }
    std::string action = lower(parsed.value("action", "reason"));
    if (action != "inspect_context" && action != "reason" && action != "draft" && action != "final") {
        action = force_final ? "final" : "reason";
    }
    return {action, limit_text(parsed.value("observation", ""), 1400),
            trim(parsed.value("answer", ""))};
}

std::string fallback_memory(const std::string& previous,
                            const ParsedReact& round,
                            int index,
                            bool enable_compression) {
    const std::string updated = previous + "\n第" + std::to_string(index) +
                                "轮: action=" + round.action +
                                "; observation=" + round.observation +
                                "; answer=" + round.answer;
    if (!enable_compression) return updated;
    return limit_text(updated, 2200);
}

}  // namespace

json AgentDefinition::public_json() const {
    return {{"id", id}, {"name", name}, {"role", role}, {"icon", icon}};
}

json PlanItem::to_json() const {
    return {{"agent_id", agent_id}, {"subtask", subtask}, {"depends_on", depends_on}};
}

json TraceEvent::to_json() const {
    return {{"type", type}, {"title", title}, {"payload", payload},
            {"seq", seq}, {"ts", timestamp_ms / 1000.0}, {"schema_version", "1"}};
}

json SkillActivation::public_json() const {
    return {{"id", skill.id}, {"name", skill.name}, {"description", skill.description},
            {"score", score}, {"reason", reason}};
}

void ToolRegistry::add(ToolDefinition definition, ToolExecutor executor) {
    if (definition.name.empty() || !executor) throw std::invalid_argument("invalid tool definition");
    entries_[definition.name] = {std::move(definition), std::move(executor)};
}

std::vector<ToolDefinition> ToolRegistry::definitions() const {
    std::vector<ToolDefinition> result;
    for (const auto& [_, entry] : entries_) result.push_back(entry.definition);
    return result;
}

ToolResult ToolRegistry::execute(const std::string& name, const std::string& arguments_json) const {
    const auto it = entries_.find(name);
    if (it == entries_.end()) return {false, {}, "tool is not registered: " + name};
    return it->second.executor(name, arguments_json);
}

bool ToolRegistry::empty() const { return entries_.empty(); }

QwenChatModel::QwenChatModel(std::string api_key, std::string model, std::string endpoint)
    : api_key_(std::move(api_key)), model_(std::move(model)), endpoint_(std::move(endpoint)) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

QwenChatModel::~QwenChatModel() { curl_global_cleanup(); }

std::string QwenChatModel::complete(const std::vector<ChatMessage>& messages, double temperature) {
    if (api_key_.empty()) throw std::runtime_error("QWEN_API_KEY is required");
    json body = {{"model", model_}, {"temperature", temperature}, {"messages", json::array()}};
    for (const auto& message : messages) {
        // Protect the HTTP request boundary as well as the diagnostic logger.
        // Materials may bypass limit_text and can still contain malformed
        // bytes, which nlohmann::json refuses to store in a string.
        body["messages"].push_back({{"role", json_safe_text(message.role)},
                                     {"content", json_safe_text(message.content)}});
    }

    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("failed to initialize CURL");
    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    const std::string authorization = "Authorization: Bearer " + api_key_;
    headers = curl_slist_append(headers, authorization.c_str());
    curl_easy_setopt(curl, CURLOPT_URL, endpoint_.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    const std::string request_body = body.dump();
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 90L);
    const CURLcode code = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (code != CURLE_OK) throw std::runtime_error(std::string("Qwen request failed: ") + curl_easy_strerror(code));

    const auto parsed = safe_json_parse(response);
    if (!parsed.is_object() || parsed.contains("error")) {
        throw std::runtime_error("Qwen API returned an error");
    }
    if (!parsed.contains("choices") || !parsed["choices"].is_array() || parsed["choices"].empty()) {
        throw std::runtime_error("Qwen API returned an invalid response");
    }
    return output_text(parsed["choices"][0].value("message", json::object()));
}

LambdaChatModel::LambdaChatModel(Handler handler) : handler_(std::move(handler)) {}

std::string LambdaChatModel::complete(const std::vector<ChatMessage>& messages, double temperature) {
    return handler_(messages, temperature);
}

std::vector<AgentDefinition> default_agents() {
    return {
        {"researcher", "Researcher", "Research facts, context, source clues, and open questions.", "R", {}},
        {"designer", "Designer", "Design structure, comparisons, information hierarchy, and presentation.", "D", {}},
        {"analyst", "Analyst", "Analyze evidence, patterns, tradeoffs, and implications.", "A", {}},
        {"strategist", "Strategist", "Turn findings into recommendations, priorities, and risks.", "S", {}},
        {"writer", "Writer", "Integrate upstream work into a clear, useful final response.", "W", {}}
    };
}

std::vector<SkillSpec> default_skills() {
    return {
        {"topic_research", "Topic Research", "Research, source clues, audience questions, and angle discovery.",
         "Clarify the target audience, collect useful source clues, surface open questions, and separate facts from assumptions.",
         {"topic", "research", "source", "audience", "选题", "调研", "素材"}, false},
        {"content_structure", "Content Structure", "Outline, narrative flow, information hierarchy, and content framing.",
         "Turn raw material into a clear outline with a strong opening, coherent sections, and a concrete takeaway.",
         {"outline", "structure", "design", "report", "大纲", "结构", "框架"}, false},
        {"brand_voice", "Brand Voice", "Tone, clarity, style consistency, editing, and brand-safe wording.",
         "Keep the wording concise, credible, and consistent; remove vague claims and fit the intended channel.",
         {"write", "writer", "voice", "brand", "tone", "撰写", "写作", "审校", "语气"}, false},
        {"platform_adaptation", "Platform Adaptation", "Rewrite one idea for publishing channels and formats.",
         "Adapt the message into channel-specific titles, hooks, lengths, hashtags, and calls to action.",
         {"platform", "publish", "wechat", "linkedin", "twitter", "小红书", "公众号", "发布"}, false}
    };
}

std::string build_agent_design_prompt(int max_agents, const std::vector<std::string>& references) {
    std::ostringstream out;
    out << "You are an Agent organization designer for a dynamic DAG multi-agent orchestrator. "
           "Given the user's task, create a small task-specific team.\n\n";
    if (!references.empty()) out << "Supplemental reference materials:\n" << render_materials(references) << "\n";
    out << "Return only a JSON array. Do not wrap it in markdown. Each item must use this schema:\n"
           "{\"id\":\"safe_snake_case_id\",\"name\":\"short role name\","
           "\"role\":\"what this agent is responsible for\",\"icon\":\"1-2 uppercase letters\","
           "\"skills\":[\"short_skill_id\"],\"output_contract\":\"what this agent must produce\"}\n\n"
           "Rules:\n"
           "1. Create between 3 and " << max_agents << " agents.\n"
           "2. Include one final synthesis/writer/reviewer agent that integrates upstream outputs.\n"
           "3. Prefer specialized roles that fit the task instead of generic titles.\n"
           "4. Do not generate code, tools, URLs, secrets, or executable instructions.\n"
           "5. Agent ids must be lowercase snake_case and unique.\n"
           "6. Keep roles concise but specific enough for a later planner to build a DAG.";
    return out.str();
}

std::string build_planner_prompt(const std::vector<AgentDefinition>& agents,
                                 const std::vector<std::string>& references) {
    std::ostringstream out;
    out << "You are a multi-agent orchestrator. Split the user's task into a JSON array execution plan. "
           "Available agents:\n\n";
    for (const auto& agent : agents) out << "- " << agent.id << ": " << agent.name << " (" << agent.role << ")\n";
    if (!references.empty()) {
        out << "\nSupplemental reference materials supplied by the user. Treat them as task context and domain constraints. "
               "Do not invent facts beyond them:\n\n" << render_materials(references);
    }
    out << "\nRules:\n"
           "1. Return only a JSON array. Do not wrap it in markdown.\n"
           "2. Each item must have agent_id, subtask, and depends_on.\n"
           "3. depends_on lists agent ids that must finish first.\n"
           "4. Assign each agent at most once.\n"
           "5. Choose serial, parallel, or mixed DAGs based on the task.\n"
           "6. Use only the available agent ids.";
    return out.str();
}

std::vector<AgentDefinition> parse_agent_design(const std::string& raw,
                                                const std::string& user_input,
                                                int max_agents) {
    const auto parsed = safe_json_parse(extract_array(raw));
    std::vector<AgentDefinition> result;
    std::set<std::string> seen;
    if (parsed.is_array()) {
        for (const auto& item : parsed) {
            if (!item.is_object()) continue;
            const std::string id = safe_id(item.value("id", ""), "agent_" + std::to_string(result.size() + 1));
            if (seen.count(id)) continue;
            AgentDefinition agent;
            agent.id = id;
            agent.name = limit_text(trim(item.value("name", id)), 80);
            agent.role = limit_text(trim(item.value("role", "Complete a specialized part of the user task.")), 420);
            agent.icon = limit_text(trim(item.value("icon", "A")), 2);
            if (agent.icon.empty()) agent.icon = "A";
            if (item.contains("skills") && item["skills"].is_array()) {
                for (const auto& skill : item["skills"]) {
                    if (skill.is_string()) agent.role += " Skill focus: " + skill.get<std::string>() + ".";
                }
            }
            result.push_back(std::move(agent));
            seen.insert(id);
            if (static_cast<int>(result.size()) >= max_agents) break;
        }
    }
    if (result.size() >= 2) return result;
    const auto fallback = default_agents();
    (void)user_input;
    result.assign(fallback.begin(), fallback.begin() + std::min<std::size_t>(fallback.size(), max_agents));
    return result;
}

std::vector<PlanItem> parse_plan(const std::string& raw,
                                 const std::vector<AgentDefinition>& agents,
                                 const std::string& user_input) {
    const auto parsed = safe_json_parse(extract_array(raw));
    std::set<std::string> valid;
    for (const auto& agent : agents) valid.insert(agent.id);
    std::vector<PlanItem> result;
    std::set<std::string> seen;
    if (parsed.is_array()) {
        for (const auto& item : parsed) {
            if (!item.is_object()) continue;
            const std::string id = trim(item.value("agent_id", ""));
            if (!valid.count(id) || seen.count(id)) continue;
            PlanItem plan{id, trim(item.value("subtask", "")), {}};
            if (plan.subtask.empty()) plan.subtask = "Complete the user task: " + user_input;
            if (item.contains("depends_on") && item["depends_on"].is_array()) {
                for (const auto& dep : item["depends_on"]) {
                    if (!dep.is_string()) continue;
                    const auto dep_id = trim(dep.get<std::string>());
                    if (valid.count(dep_id) && std::find(plan.depends_on.begin(), plan.depends_on.end(), dep_id) == plan.depends_on.end()) {
                        plan.depends_on.push_back(dep_id);
                    }
                }
            }
            result.push_back(std::move(plan));
            seen.insert(id);
        }
    }
    if (!result.empty()) return result;
    for (const auto& agent : agents) {
        result.push_back({agent.id, "Please complete this task from the perspective of " + agent.role + ": " + user_input, {}});
    }
    return result;
}

struct DagRuntime::RunState {
    mutable std::mutex mutex;
    std::string id;
    RunOptions options;
    std::vector<AgentDefinition> agents;
    std::vector<PlanItem> plan;
    std::map<std::string, std::string> outputs;
    std::map<std::string, std::string> errors;
    std::map<std::string, std::string> statuses;
    std::map<std::string, std::string> edited_prior;
    std::map<std::string, std::string> feedback;
    std::vector<TraceEvent> events;
    std::int64_t next_seq = 0;
};

DagRuntime::DagRuntime(std::shared_ptr<ChatModel> model,
                       std::filesystem::path log_directory)
    : model_(std::move(model)),
      default_agents_(default_agents()),
      skills_(default_skills()),
      log_directory_(std::move(log_directory)) {
    if (!log_directory_.empty()) {
        std::error_code error;
        std::filesystem::create_directories(log_directory_, error);
        if (error) {
            std::cerr << "[DAG] cannot create log directory " << log_directory_ << ": "
                      << error.message() << std::endl;
        }
    }
}

std::vector<AgentDefinition> DagRuntime::agents() const { return default_agents_; }

void DagRuntime::set_default_agents(std::vector<AgentDefinition> agents) {
    default_agents_ = std::move(agents);
}

void DagRuntime::log_chat_exchange(const std::string& log_file,
                                   const std::string& phase,
                                   const std::string& run_id,
                                   const std::string& agent_id,
                                   int round,
                                   double temperature,
                                   const std::vector<ChatMessage>& messages,
    const std::string& response,
                                   const std::string& error) const {
    if (log_directory_.empty()) return;

    std::lock_guard<std::mutex> lock(log_mutex_);
    try {
        json serialized_messages = json::array();
        for (const auto& message : messages) {
            serialized_messages.push_back({{"role", json_safe_text(message.role)},
                                           {"content", json_safe_text(message.content)}});
        }
        json record = {
            {"timestamp_ms", now_ms()},
            {"phase", json_safe_text(phase)},
            {"run_id", json_safe_text(run_id)},
            {"agent_id", json_safe_text(agent_id)},
            {"round", round},
            {"temperature", temperature},
            {"request", {{"messages", serialized_messages}}}
        };
        if (!response.empty()) record["response"] = json_safe_text(response);
        if (!error.empty()) record["error"] = json_safe_text(error);

        std::error_code directory_error;
        std::filesystem::create_directories(log_directory_, directory_error);
        if (directory_error) {
            std::cerr << "[DAG] cannot create log directory " << log_directory_ << ": "
                      << directory_error.message() << std::endl;
            return;
        }
        std::ofstream output(log_directory_ / safe_log_file_name(log_file),
                             std::ios::out | std::ios::app);
        if (!output) {
            std::cerr << "[DAG] cannot open log file "
                      << (log_directory_ / safe_log_file_name(log_file)) << std::endl;
            return;
        }
        output << record.dump(2) << "\n";
    } catch (const std::exception& exception) {
        // Logging must never turn a successful Agent execution into a failed
        // DAG node. The service log still records the file-system problem.
        std::cerr << "[DAG] writing chat log failed: " << exception.what() << std::endl;
    }
}

std::string DagRuntime::create_run(RunOptions options) {
    if (trim(options.user_input).empty()) throw std::invalid_argument("user_input is required");
    if (options.agents.empty() && !options.auto_agents) options.agents = default_agents_;
    options.max_dynamic_agents = std::max(3, std::min(options.max_dynamic_agents, 10));
    options.max_agent_rounds = std::max(1, std::min(options.max_agent_rounds, 10));

    auto state = std::make_shared<RunState>();
    state->id = [&] {
        std::ostringstream out;
        out << std::hex << now_ms() << std::this_thread::get_id();
        return out.str().substr(0, 12);
    }();
    state->options = std::move(options);
    state->agents = state->options.agents;
    for (const auto& agent : state->agents) state->statuses[agent.id] = "pending";
    {
        std::lock_guard<std::mutex> lock(runs_mutex_);
        runs_[state->id] = state;
    }
    emit(state, "run_created", "Run created", {{"run_id", state->id}, {"agent_mode", state->options.auto_agents ? "auto" : "manual"}});
    std::thread([this, state] { execute_run(state); }).detach();
    return state->id;
}

json DagRuntime::snapshot(const std::string& run_id) const {
    std::shared_ptr<RunState> state;
    {
        std::lock_guard<std::mutex> lock(runs_mutex_);
        const auto it = runs_.find(run_id);
        if (it == runs_.end()) return json();
        state = it->second;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    json result = {
        {"run_id", state->id},
        {"user_input", state->options.user_input},
        {"agent_ids", json::array()},
        {"assignments", json::object()},
        {"depends_on", json::object()},
        {"outputs", state->outputs},
        {"statuses", state->statuses},
        {"errors", state->errors},
        {"plan", json::array()},
        {"events", json::array()},
        {"reference_material_count", state->options.reference_materials.size()},
        {"agent_reference_material_counts", json::object()}
    };
    for (const auto& agent : state->agents) {
        result["agent_ids"].push_back(agent.id);
        result["agent_reference_material_counts"][agent.id] = agent.private_materials.size();
    }
    for (const auto& item : state->plan) {
        result["plan"].push_back(item.to_json());
        result["assignments"][item.agent_id] = item.subtask;
        result["depends_on"][item.agent_id] = item.depends_on;
    }
    for (const auto& event : state->events) result["events"].push_back(event.to_json());
    return result;
}

bool DagRuntime::retry_agent(const std::string& run_id,
                             const std::string& agent_id,
                             const std::string& edited_prior,
                             const std::string& user_feedback) {
    std::shared_ptr<RunState> state;
    {
        std::lock_guard<std::mutex> lock(runs_mutex_);
        const auto it = runs_.find(run_id);
        if (it == runs_.end()) return false;
        state = it->second;
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        const auto found = std::find_if(state->agents.begin(), state->agents.end(), [&](const auto& agent) { return agent.id == agent_id; });
        if (found == state->agents.end()) return false;
        state->edited_prior[agent_id] = edited_prior;
        state->feedback[agent_id] = user_feedback;
        state->statuses[agent_id] = "running";
    }
    emit(state, "retry_started", "Retry started", {{"agent_id", agent_id}});
    std::thread([this, state, agent_id] { execute_retry(state, agent_id); }).detach();
    return true;
}

void DagRuntime::emit(const std::shared_ptr<RunState>& state,
                      const std::string& type,
                      const std::string& title,
                      json payload) const {
    TraceEvent event{type, title, std::move(payload), 0, now_ms()};
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        event.seq = ++state->next_seq;
        state->events.push_back(event);
    }
    std::cout << "[DAG][" << state->id << "] " << type << ": " << title << std::endl;
}

void DagRuntime::execute_run(const std::shared_ptr<RunState>& state) {
    try {
        if (state->options.auto_agents && state->agents.empty()) {
            std::string raw;
            const std::vector<ChatMessage> messages = {
                {"system", build_agent_design_prompt(state->options.max_dynamic_agents,
                                                       state->options.reference_materials)},
                {"user", state->options.user_input}
            };
            try {
                raw = model_->complete(messages, 0.35);
                log_chat_exchange("agent_designer.log", "agent_design", state->id, "", 0,
                                  0.35, messages, raw);
            } catch (const std::exception& error) {
                log_chat_exchange("agent_designer.log", "agent_design", state->id, "", 0,
                                  0.35, messages, {}, error.what());
                std::cerr << "[DAG][" << state->id << "] Agent design failed: " << error.what() << std::endl;
            }
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->agents = parse_agent_design(raw, state->options.user_input, state->options.max_dynamic_agents);
                for (auto& agent : state->agents) {
                    const auto it = state->options.agent_reference_materials.find(agent.id);
                    if (it != state->options.agent_reference_materials.end()) agent.private_materials = it->second;
                }
                state->statuses.clear();
                for (const auto& agent : state->agents) state->statuses[agent.id] = "pending";
            }
            emit(state, "agents_designed", "Agents designed", {{"agents", [&] { json a = json::array(); for (const auto& agent : state->agents) a.push_back(agent.public_json()); return a; }()}});
        }

        std::string raw_plan;
        const std::vector<ChatMessage> planner_messages = {
            {"system", build_planner_prompt(state->agents, state->options.reference_materials)},
            {"user", state->options.user_input}
        };
        try {
            raw_plan = model_->complete(planner_messages, 0.3);
            log_chat_exchange("planner.log", "planner", state->id, "", 0, 0.3,
                              planner_messages, raw_plan);
        } catch (const std::exception& error) {
            log_chat_exchange("planner.log", "planner", state->id, "", 0, 0.3,
                              planner_messages, {}, error.what());
            std::cerr << "[DAG][" << state->id << "] Planner failed, using fallback plan: " << error.what() << std::endl;
        }
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->plan = parse_plan(raw_plan, state->agents, state->options.user_input);
            state->statuses.clear();
            for (const auto& item : state->plan) state->statuses[item.agent_id] = "pending";
        }

        std::set<std::string> ids;
        bool valid = true;
        for (const auto& item : state->plan) {
            if (!ids.insert(item.agent_id).second) valid = false;
            for (const auto& dep : item.depends_on) if (dep == item.agent_id || !std::any_of(state->plan.begin(), state->plan.end(), [&](const auto& other) { return other.agent_id == dep; })) valid = false;
        }
        std::map<std::string, std::vector<std::string>> dependencies;
        for (const auto& item : state->plan) dependencies[item.agent_id] = item.depends_on;
        std::function<bool(const std::string&, std::set<std::string>&, std::set<std::string>&)> visit =
            [&](const std::string& id, std::set<std::string>& visiting, std::set<std::string>& visited) {
                if (visiting.count(id)) return true;
                if (visited.count(id)) return false;
                visiting.insert(id);
                for (const auto& dep : dependencies[id]) if (visit(dep, visiting, visited)) return true;
                visiting.erase(id);
                visited.insert(id);
                return false;
            };
        std::set<std::string> visiting;
        std::set<std::string> visited;
        for (const auto& item : state->plan) if (visit(item.agent_id, visiting, visited)) valid = false;
        emit(state, "plan_created", "Execution plan created", {{"plan", [&] { json p = json::array(); for (const auto& item : state->plan) p.push_back(item.to_json()); return p; }()}, {"valid", valid}});
        if (!valid || state->plan.empty()) {
            emit(state, "run_stalled", "Run stalled by invalid plan");
            return;
        }

        std::set<std::string> completed;
        std::set<std::string> failed;
        while (completed.size() + failed.size() < state->plan.size()) {
            std::vector<PlanItem> ready;
            for (const auto& item : state->plan) {
                if (completed.count(item.agent_id) || failed.count(item.agent_id)) continue;
                bool dependencies_done = true;
                for (const auto& dep : item.depends_on) {
                    if (!completed.count(dep)) { dependencies_done = false; break; }
                }
                if (dependencies_done) ready.push_back(item);
            }
            if (ready.empty()) {
                for (const auto& item : state->plan) {
                    if (!completed.count(item.agent_id) && !failed.count(item.agent_id)) {
                        std::lock_guard<std::mutex> lock(state->mutex);
                        state->statuses[item.agent_id] = "stalled";
                    }
                }
                emit(state, "run_stalled", "Run stalled", {{"stalled", [&] { json s = json::array(); for (const auto& item : state->plan) if (!completed.count(item.agent_id) && !failed.count(item.agent_id)) s.push_back(item.agent_id); return s; }()}});
                return;
            }
            std::vector<std::future<std::pair<std::string, bool>>> futures;
            for (const auto& node : ready) {
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->statuses[node.agent_id] = "running";
                }
                emit(state, "node_queued", "Node queued", {{"agent_id", node.agent_id}, {"depends_on", node.depends_on}});
                futures.push_back(std::async(std::launch::async, [this, state, node] {
                    std::string error;
                    const bool ok = execute_node(state, node, &error);
                    return std::make_pair(node.agent_id, ok);
                }));
            }
            for (auto& future : futures) {
                const auto [id, ok] = future.get();
                if (ok) {
                    completed.insert(id);
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->statuses[id] = "done";
                } else {
                    failed.insert(id);
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->statuses[id] = "error";
                }
            }
        }
        emit(state, "run_done", "Run done", {{"completed", [&] { json s = json::array(); for (const auto& id : completed) s.push_back(id); return s; }()}, {"failed", [&] { json s = json::array(); for (const auto& id : failed) s.push_back(id); return s; }()}});
    } catch (const std::exception& error) {
        emit(state, "run_stalled", "Run failed", {{"message", error.what()}});
    }
}

void DagRuntime::execute_retry(const std::shared_ptr<RunState>& state, const std::string& agent_id) {
    PlanItem node;
    AgentDefinition agent;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        const auto plan_it = std::find_if(state->plan.begin(), state->plan.end(), [&](const auto& item) { return item.agent_id == agent_id; });
        const auto agent_it = std::find_if(state->agents.begin(), state->agents.end(), [&](const auto& value) { return value.id == agent_id; });
        if (plan_it == state->plan.end() || agent_it == state->agents.end()) return;
        node = *plan_it;
        agent = *agent_it;
    }
    std::string error;
    const bool ok = execute_node(state, node, &error);
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->statuses[agent_id] = ok ? "done" : "error";
    }
    emit(state, "retry_done", ok ? "Retry done" : "Retry failed", {{"agent_id", agent_id}, {"ok", ok}});
}

bool DagRuntime::execute_node(const std::shared_ptr<RunState>& state,
                              const PlanItem& node,
                              std::string* error) {
    AgentDefinition agent;
    std::map<std::string, std::string> dependency_outputs;
    std::string edited_prior;
    std::string feedback;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        const auto agent_it = std::find_if(state->agents.begin(), state->agents.end(), [&](const auto& value) { return value.id == node.agent_id; });
        if (agent_it == state->agents.end()) { *error = "unknown agent"; return false; }
        agent = *agent_it;
        for (const auto& dep : node.depends_on) dependency_outputs[dep] = state->outputs[dep];
        edited_prior = state->edited_prior[node.agent_id];
        feedback = state->feedback[node.agent_id];
    }
    const auto active_skills = select_skills(state->options.user_input, node, agent);
    if (!active_skills.empty()) {
        json skills = json::array();
        for (const auto& skill : active_skills) skills.push_back(skill.public_json());
        emit(state, "skills_activated", agent.name + " skills activated", {{"agent_id", agent.id}, {"skills", skills}});
    }
    emit(state, "node_entered", agent.name + " started", {{"agent_id", agent.id}, {"subtask", node.subtask}});
    try {
        const auto output = run_react(state, node, agent, active_skills);
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->outputs[agent.id] = output;
            state->errors.erase(agent.id);
        }
        emit(state, "node_exited", agent.name + " completed", {{"agent_id", agent.id}, {"output", output}});
        return true;
    } catch (const std::exception& exception) {
        *error = exception.what();
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->errors[agent.id] = *error;
        }
        emit(state, "node_error", agent.name + " failed", {{"agent_id", agent.id}, {"message", *error}});
        return false;
    }
}

std::vector<SkillActivation> DagRuntime::select_skills(const std::string& user_input,
                                                       const PlanItem& node,
                                                       const AgentDefinition& agent) const {
    const auto query = token_set(user_input + " " + node.subtask + " " + agent.name + " " + agent.role);
    std::vector<SkillActivation> ranked;
    for (const auto& skill : skills_) {
        double score = skill.always_on ? 1.0 : 0.0;
        std::vector<std::string> matches;
        std::set<std::string> skill_tokens = token_set(skill.id + " " + skill.name + " " + skill.description + " " + join(skill.keywords, " "));
        for (const auto& token : query) if (skill_tokens.count(token)) score += 1.0;
        const auto lowered_query = lower(user_input + " " + node.subtask);
        for (const auto& keyword : skill.keywords) if (!keyword.empty() && lowered_query.find(lower(keyword)) != std::string::npos) { score += 2.0; matches.push_back(keyword); }
        if (score > 0) ranked.push_back({skill, score, matches.empty() ? (skill.always_on ? "always_on" : "metadata_match") : "keyword:" + join(matches, ",")});
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
        if (left.score != right.score) return left.score > right.score;
        return left.skill.id < right.skill.id;
    });
    if (ranked.size() > 3) ranked.resize(3);
    return ranked;
}

std::string DagRuntime::run_react(const std::shared_ptr<RunState>& state,
                                  const PlanItem& node,
                                  const AgentDefinition& agent,
                                  const std::vector<SkillActivation>& skills) {
    std::map<std::string, std::string> dependency_outputs;
    std::string edited_prior;
    std::string feedback;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        for (const auto& dep : node.depends_on) dependency_outputs[dep] = state->outputs[dep];
        edited_prior = state->edited_prior[node.agent_id];
        feedback = state->feedback[node.agent_id];
    }
    std::string memory = "User task: " + state->options.user_input + "\nCurrent subtask: " + node.subtask +
                         "\nReferences:\n" + render_materials([&] {
                             std::vector<std::string> references = state->options.reference_materials;
                             references.insert(references.end(), agent.private_materials.begin(), agent.private_materials.end());
                             return references;
                         }()) +
                         "\nUpstream outputs:\n" + compact_dependencies(dependency_outputs) +
                         "\nActive skills:\n" + compact_skills(skills);
    std::string final_answer;
    for (int round = 1; round <= state->options.max_agent_rounds; ++round) {
        const bool force_final = round >= state->options.max_agent_rounds;
        const std::string system_prompt =
            "你是一个多 Agent DAG 工作流中的 ReAct 执行 Agent。\n"
            "你的身份是：" + agent.name + "\n你的职责是：" + agent.role + "\n\n"
            "你需要按 Thought -> Action -> Observation -> Answer 的方式推进任务。"
            "Thought 只写简短推理摘要，不要展开冗长内心独白。\n\n"
            "输出要求：\n1. 使用中文。\n2. 每轮只返回 JSON 对象，不要 markdown，不要代码块。\n"
            "3. action 只能是 inspect_context、reason、draft、final 之一。\n"
            "4. action=final 时必须给出完整可用的最终答案。\n"
            "5. 不要输出空泛模板，不要用需要工具来逃避任务。\n"
            "6. 当前没有可用工具，不要虚构工具结果。";
        const std::string user_prompt =
            "用户原始任务：\n" + state->options.user_input +
            "\n\n当前 Agent 子任务：\n" + node.subtask +
            "\n\n上游依赖输出：\n" + compact_dependencies(dependency_outputs) +
            "\n\n用户补充资料：\n" + render_materials([&] {
                std::vector<std::string> references = state->options.reference_materials;
                references.insert(references.end(), agent.private_materials.begin(), agent.private_materials.end());
                return references;
            }()) +
            "\n\n已激活技能：\n" + compact_skills(skills) +
            (edited_prior.empty() && feedback.empty() ? "" : "\n\n用户修改后的上一版输出：\n" + edited_prior + "\n\n用户反馈：\n" + feedback) +
            "\n\nReAct 累积记忆（上下文压缩：" +
            std::string(state->options.enable_memory_compression ? "开启" : "关闭") +
            "）：\n" + memory +
            "\n\n当前轮次：" + std::to_string(round) + "/" + std::to_string(state->options.max_agent_rounds) +
            "\n本轮是否必须 final：" + std::string(force_final ? "是" : "否") +
            "\n\n返回 JSON schema：{\"thought\":\"简短推理摘要\",\"action\":\"inspect_context | reason | draft | final\",\"observation\":\"关键事实或结论\",\"answer\":\"最终答案或空字符串\"}";
        const std::vector<ChatMessage> messages = {{"system", system_prompt}, {"user", user_prompt}};
        std::string raw;
        try {
            raw = model_->complete(messages, 0.35);
            log_chat_exchange("agent_" + agent.id,
                              "react", state->id, agent.id, round, 0.35, messages, raw);
        } catch (const std::exception& error) {
            log_chat_exchange("agent_" + agent.id,
                              "react", state->id, agent.id, round, 0.35, messages, {}, error.what());
            throw;
        }
        const auto parsed = parse_react(raw, force_final);
        if (!parsed.answer.empty()) final_answer = parsed.answer;
        memory = fallback_memory(memory, parsed, round,
                                 state->options.enable_memory_compression);
        if (parsed.action == "final" && !final_answer.empty()) break;
    }
    if (final_answer.empty()) final_answer = memory;
    return final_answer;
}

}  // namespace ai_orchestrator::dag
