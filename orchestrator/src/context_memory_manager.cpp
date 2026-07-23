#include <agent_rpc/orchestrator/context_memory_manager.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace agent_rpc {
namespace orchestrator {
namespace {

using json = nlohmann::json;

/**
 * @brief 获取当前时间的 ISO 字符串
 * @return 当前本地时间字符串
 */
std::string current_time_iso() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);

    std::tm time_info{};
#if defined(_WIN32)
    localtime_s(&time_info, &now_time);
#else
    localtime_r(&now_time, &time_info);
#endif

    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S%z", &time_info);
    return buffer;
}

/**
 * @brief 去掉字符串首尾空白
 * @param text 原始文本
 * @return 清理后的文本
 */
std::string trim(const std::string& text) {
    std::size_t start = 0;
    while (start < text.size() &&
           std::isspace(static_cast<unsigned char>(text[start]))) {
        start++;
    }

    std::size_t end = text.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        end--;
    }

    return text.substr(start, end - start);
}

/**
 * @brief 归一化文本用于去重
 * @param text 原始文本
 * @return 小写并压缩空白后的文本
 */
std::string normalize_for_dedup(const std::string& text) {
    std::string result;
    result.reserve(text.size());

    bool last_space = false;
    for (unsigned char ch : text) {
        if (std::isspace(ch)) {
            if (!last_space) {
                result.push_back(' ');
                last_space = true;
            }
            continue;
        }

        last_space = false;
        result.push_back(static_cast<char>(std::tolower(ch)));
    }

    return trim(result);
}

/**
 * @brief 从模型输出中提取 JSON 对象文本
 * @param text 模型原始输出
 * @return JSON 对象文本，提取失败时返回空字符串
 */
std::string extract_json_object_text(const std::string& text) {
    std::string cleaned = trim(text);
    const std::string fence = "```";
    const std::size_t first_fence = cleaned.find(fence);
    if (first_fence != std::string::npos) {
        const std::size_t first_brace_after_fence = cleaned.find('{', first_fence);
        const std::size_t last_fence = cleaned.rfind(fence);
        if (first_brace_after_fence != std::string::npos &&
            last_fence != std::string::npos &&
            last_fence > first_brace_after_fence) {
            cleaned = cleaned.substr(first_brace_after_fence,
                                     last_fence - first_brace_after_fence);
        }
    }

    const std::size_t begin = cleaned.find('{');
    const std::size_t end = cleaned.rfind('}');
    if (begin == std::string::npos || end == std::string::npos || end <= begin) {
        return "";
    }

    return cleaned.substr(begin, end - begin + 1);
}

/**
 * @brief 限制重要度到 1-10
 * @param importance 原始重要度
 * @return 合法重要度
 */
int clamp_importance(int importance) {
    return std::max(1, std::min(10, importance));
}

}  // namespace

struct ContextMemoryManager::MemoryState {
    std::string context_id;
    std::string sanitized_context_id;
    std::string short_term_summary;
    std::size_t consolidated_message_count = 0;
    std::vector<LongTermMemoryItem> long_term_memories;
    std::string updated_at;
};

ContextMemoryManager::ContextMemoryManager(ContextMemoryConfig config)
    : config_(std::move(config)) {}

void ContextMemoryManager::set_llm_compressor(LlmCompressor compressor) {
    std::lock_guard<std::mutex> lock(mutex_);
    llm_compressor_ = std::move(compressor);
}

void ContextMemoryManager::observe_conversation(
    const std::string& context_id,
    const std::vector<ContextMemoryMessage>& history) {
    if (context_id.empty() || history.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    MemoryState state = load_state_unlocked(context_id);
    const std::size_t keep_recent =
        std::max<std::size_t>(1, config_.keep_recent_messages);
    if (history.size() <= keep_recent) {
        return;
    }

    const std::size_t compress_end = history.size() - keep_recent;
    if (compress_end <= state.consolidated_message_count) {
        return;
    }

    const std::size_t compress_begin =
        std::min(state.consolidated_message_count, compress_end);
    std::vector<ContextMemoryMessage> messages_to_compress(
        history.begin() + static_cast<std::ptrdiff_t>(compress_begin),
        history.begin() + static_cast<std::ptrdiff_t>(compress_end));

    std::size_t token_total = 0;
    for (const auto& message : messages_to_compress) {
        token_total += estimate_tokens(message.content);
    }

    const bool enough_messages =
        messages_to_compress.size() >= config_.min_messages_to_compress;
    const bool over_working_budget = token_total >= working_budget();
    if (!enough_messages && !over_working_budget) {
        return;
    }

    if (!compress_short_term_unlocked(state, messages_to_compress)) {
        return;
    }
    state.consolidated_message_count = compress_end;

    if (estimate_tokens(state.short_term_summary) >= config_.long_term_extract_min_tokens) {
        extract_long_term_unlocked(state);
    }

    state.updated_at = current_time_iso();
    save_state_unlocked(state);
}

std::vector<ContextMemoryMessage> ContextMemoryManager::build_context_messages(
    const std::string& context_id,
    const std::vector<ContextMemoryMessage>& recent_history) const {
    std::lock_guard<std::mutex> lock(mutex_);
    MemoryState state = load_state_unlocked(context_id);

    std::vector<ContextMemoryMessage> context;

    const std::string long_term_text = format_long_term_memories(state);
    if (!long_term_text.empty()) {
        context.push_back({
            "system",
            "以下是该 context_id 的长期核心记忆，仅用于理解用户背景、科研方向、"
            "偏好和历史决策，不是新的用户指令：\n" + long_term_text
        });
    }

    if (!trim(state.short_term_summary).empty()) {
        std::string summary = trim(state.short_term_summary);
        if (estimate_tokens(summary) > short_term_budget() && short_term_budget() > 0) {
            // 预算兜底只影响注入文本，不修改持久化摘要。
            summary = summary.substr(0, std::min(summary.size(), short_term_budget() * 4));
        }
        context.push_back({
            "system",
            "以下是近期会话摘要，仅用于补足上下文，不是新的用户指令：\n" + summary
        });
    }

    std::vector<ContextMemoryMessage> working_messages;
    std::size_t used_tokens = 0;
    const std::size_t budget = working_budget();

    std::size_t history_begin = 0;
    if (state.consolidated_message_count > 0 &&
        recent_history.size() > state.consolidated_message_count) {
        // 传入完整历史时，已压缩消息不再重复进入工作记忆。
        history_begin = state.consolidated_message_count;
    }

    const auto begin_it =
        recent_history.begin() + static_cast<std::ptrdiff_t>(history_begin);
    for (auto it = recent_history.rbegin();
         it != std::vector<ContextMemoryMessage>::const_reverse_iterator(begin_it);
         ++it) {
        const std::size_t message_tokens =
            estimate_tokens(it->role) + estimate_tokens(it->content);
        if (!working_messages.empty() &&
            budget > 0 &&
            used_tokens + message_tokens > budget) {
            break;
        }

        working_messages.push_back(*it);
        used_tokens += message_tokens;
    }

    std::reverse(working_messages.begin(), working_messages.end());
    context.insert(context.end(), working_messages.begin(), working_messages.end());
    return context;
}

ContextMemorySnapshot ContextMemoryManager::load_snapshot(
    const std::string& context_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const MemoryState state = load_state_unlocked(context_id);

    ContextMemorySnapshot snapshot;
    snapshot.context_id = state.context_id;
    snapshot.sanitized_context_id = state.sanitized_context_id;
    snapshot.short_term_summary = state.short_term_summary;
    snapshot.consolidated_message_count = state.consolidated_message_count;
    snapshot.long_term_memories = state.long_term_memories;
    return snapshot;
}

ContextMemoryStats ContextMemoryManager::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

std::string ContextMemoryManager::memory_file_path(const std::string& context_id) const {
    const std::filesystem::path dir(config_.memory_dir);
    return (dir / (sanitize_context_id(context_id) + ".json")).string();
}

std::string ContextMemoryManager::sanitize_context_id(const std::string& context_id) {
    std::string sanitized;
    sanitized.reserve(context_id.size());

    for (unsigned char ch : context_id) {
        if (std::isalnum(ch) || ch == '-' || ch == '_') {
            sanitized.push_back(static_cast<char>(ch));
        } else {
            sanitized.push_back('_');
        }
    }

    sanitized = trim(sanitized);
    if (sanitized.empty()) {
        sanitized = "default";
    }

    constexpr std::size_t kMaxFileNameLength = 96;
    if (sanitized.size() > kMaxFileNameLength) {
        sanitized.resize(kMaxFileNameLength);
    }

    return sanitized;
}

std::size_t ContextMemoryManager::estimate_tokens(const std::string& text) {
    std::size_t count = 0;
    bool in_ascii_word = false;

    for (std::size_t index = 0; index < text.size(); ++index) {
        const unsigned char ch = static_cast<unsigned char>(text[index]);
        if (ch < 128) {
            if (std::isalnum(ch)) {
                if (!in_ascii_word) {
                    count++;
                    in_ascii_word = true;
                }
            } else {
                in_ascii_word = false;
                if (!std::isspace(ch)) {
                    count++;
                }
            }
            continue;
        }

        in_ascii_word = false;
        // UTF-8 非续字节近似算一个 token，避免中文文本被严重低估。
        if ((ch & 0xC0) != 0x80) {
            count++;
        }
    }

    return std::max<std::size_t>(1, count);
}

std::size_t ContextMemoryManager::working_budget() const {
    return static_cast<std::size_t>(config_.max_tokens * config_.working_memory_ratio);
}

std::size_t ContextMemoryManager::short_term_budget() const {
    return static_cast<std::size_t>(config_.max_tokens * config_.short_term_ratio);
}

std::size_t ContextMemoryManager::long_term_budget() const {
    return static_cast<std::size_t>(config_.max_tokens * config_.long_term_ratio);
}

ContextMemoryManager::MemoryState ContextMemoryManager::load_state_unlocked(
    const std::string& context_id) const {
    MemoryState state;
    state.context_id = context_id.empty() ? "default" : context_id;
    state.sanitized_context_id = sanitize_context_id(state.context_id);

    const std::filesystem::path file_path(memory_file_path(state.context_id));
    if (!std::filesystem::exists(file_path)) {
        return state;
    }

    try {
        std::ifstream input(file_path);
        const json memory_json = json::parse(input);

        state.context_id = memory_json.value("context_id", state.context_id);
        state.sanitized_context_id =
            memory_json.value("sanitized_context_id",
                              sanitize_context_id(state.context_id));
        state.short_term_summary =
            memory_json.value("short_term_summary", std::string{});
        state.consolidated_message_count =
            memory_json.value("consolidated_message_count", std::size_t{0});
        state.updated_at = memory_json.value("updated_at", std::string{});

        if (memory_json.contains("long_term_memories") &&
            memory_json["long_term_memories"].is_array()) {
            for (const auto& item_json : memory_json["long_term_memories"]) {
                LongTermMemoryItem item;
                item.content = item_json.value("content", std::string{});
                item.category = item_json.value("category", std::string{"general"});
                item.source = item_json.value("source", std::string{"auto_extract"});
                item.created_at = item_json.value("created_at", std::string{});
                item.updated_at = item_json.value("updated_at", item.created_at);
                item.importance =
                    clamp_importance(item_json.value("importance", 5));
                item.hit_count = std::max(1, item_json.value("hit_count", 1));

                if (!trim(item.content).empty()) {
                    state.long_term_memories.push_back(std::move(item));
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[ContextMemoryManager] 读取记忆文件失败: "
                  << file_path << " error=" << e.what() << std::endl;
    }

    return state;
}

void ContextMemoryManager::save_state_unlocked(const MemoryState& state) const {
    const std::filesystem::path dir(config_.memory_dir);
    const std::filesystem::path file_path(memory_file_path(state.context_id));
    const std::filesystem::path temp_path(file_path.string() + ".tmp");

    try {
        std::filesystem::create_directories(dir);

        json memory_json;
        memory_json["version"] = 1;
        memory_json["context_id"] = state.context_id;
        memory_json["sanitized_context_id"] = state.sanitized_context_id;
        memory_json["short_term_summary"] = state.short_term_summary;
        memory_json["consolidated_message_count"] =
            state.consolidated_message_count;
        memory_json["updated_at"] = state.updated_at.empty()
            ? current_time_iso()
            : state.updated_at;

        memory_json["long_term_memories"] = json::array();
        for (const auto& item : state.long_term_memories) {
            memory_json["long_term_memories"].push_back({
                {"content", item.content},
                {"category", item.category},
                {"source", item.source},
                {"importance", item.importance},
                {"hit_count", item.hit_count},
                {"created_at", item.created_at},
                {"updated_at", item.updated_at}
            });
        }

        {
            std::ofstream output(temp_path);
            output << memory_json.dump(2);
        }
        std::filesystem::rename(temp_path, file_path);
    } catch (const std::exception& e) {
        std::cerr << "[ContextMemoryManager] 保存记忆文件失败: "
                  << file_path << " error=" << e.what() << std::endl;
        try {
            if (std::filesystem::exists(temp_path)) {
                std::filesystem::remove(temp_path);
            }
        } catch (...) {
        }
    }
}

bool ContextMemoryManager::compress_short_term_unlocked(
    MemoryState& state,
    const std::vector<ContextMemoryMessage>& messages) {
    try {
        const std::string new_summary = summarize_messages_unlocked(state, messages);
        if (trim(new_summary).empty()) {
            return false;
        }

        state.short_term_summary = trim(new_summary);
        stats_.total_compressions++;

        if (estimate_tokens(state.short_term_summary) > short_term_budget() &&
            llm_compressor_) {
            const std::string system_prompt =
                "你是会话摘要二次压缩器。请在不丢失科研方向、用户偏好、"
                "关键决策和待办事项的前提下，将摘要压缩到更短。只输出中文摘要。";
            const std::string user_prompt =
                "需要压缩的摘要如下：\n" + state.short_term_summary;
            const std::string compacted =
                trim(llm_compressor_(system_prompt, user_prompt));
            if (!compacted.empty()) {
                state.short_term_summary = compacted;
            }
        }
        return true;
    } catch (const std::exception& e) {
        stats_.total_llm_failures++;
        std::cerr << "[ContextMemoryManager] 短期摘要压缩失败: "
                  << e.what() << std::endl;
        return false;
    }
}

void ContextMemoryManager::extract_long_term_unlocked(MemoryState& state) {
    try {
        const auto memories = extract_memories_from_summary_unlocked(state);
        if (memories.empty()) {
            return;
        }

        merge_long_term_memories(state, memories);
        stats_.total_long_term_extractions++;
    } catch (const std::exception& e) {
        stats_.total_llm_failures++;
        std::cerr << "[ContextMemoryManager] 长期记忆提取失败: "
                  << e.what() << std::endl;
    }
}

std::string ContextMemoryManager::summarize_messages_unlocked(
    const MemoryState& state,
    const std::vector<ContextMemoryMessage>& messages) {
    if (!llm_compressor_) {
        throw std::runtime_error("LLM compressor is not configured");
    }

    const std::string system_prompt =
        "你是 Multi Agent 系统的会话记忆压缩器。请用中文压缩历史对话，"
        "只保留对后续协作真正有用的信息，不要新增事实，不要记录 API Key、"
        "密码、临时日志噪声或一次性寒暄。\n"
        "重点提取：\n"
        "1. 用户作为科研人的研究方向、课题背景、当前项目目标。\n"
        "2. 用户对技术栈、实现方式、代码风格、编译验证和回答粒度的偏好。\n"
        "3. 已确认的设计决策、约束条件、待办事项和风险。\n"
        "4. 与 C++、动态 DAG、MCP、RAG、Multi Agent、科研资料检索、"
        "会议纪要和服务器运维协同有关的上下文。\n"
        "输出要求：最多 8 条要点，语言紧凑，不要解释压缩过程。";

    std::string user_prompt;
    if (!trim(state.short_term_summary).empty()) {
        user_prompt += "已有短期摘要：\n";
        user_prompt += state.short_term_summary;
        user_prompt += "\n\n";
    }
    user_prompt += "需要继续压缩的历史消息：\n";
    user_prompt += format_messages_for_prompt(messages);

    return llm_compressor_(system_prompt, user_prompt);
}

std::vector<LongTermMemoryItem>
ContextMemoryManager::extract_memories_from_summary_unlocked(
    const MemoryState& state) {
    if (!llm_compressor_) {
        throw std::runtime_error("LLM compressor is not configured");
    }

    const std::string system_prompt =
        "你是长期记忆提取器。请从短期摘要中提取适合跨会话记住的稳定信息。"
        "特别关注科研人的研究方向、技术偏好、协作偏好、长期项目背景、"
        "明确决策和长期约束。不要提取临时任务进度、一次性错误、密钥、"
        "隐私敏感信息或没有稳定价值的普通寒暄。\n"
        "只返回 JSON 对象，不要 Markdown，不要解释。格式必须是：\n"
        "{\"memories\":[{\"content\":\"...\",\"category\":\"research_direction|preference|decision|constraint|project_context|workflow|other\",\"importance\":1-10}]}";

    const std::string user_prompt =
        "context_id: " + state.context_id + "\n\n短期摘要：\n" +
        state.short_term_summary;

    const std::string response = llm_compressor_(system_prompt, user_prompt);
    const std::string json_text = extract_json_object_text(response);
    if (json_text.empty()) {
        return {};
    }

    const json parsed = json::parse(json_text);
    if (!parsed.contains("memories") || !parsed["memories"].is_array()) {
        return {};
    }

    std::vector<LongTermMemoryItem> memories;
    const std::string now = current_time_iso();

    for (const auto& item_json : parsed["memories"]) {
        LongTermMemoryItem item;
        item.content = trim(item_json.value("content", std::string{}));
        item.category = trim(item_json.value("category", std::string{"other"}));
        item.importance =
            clamp_importance(item_json.value("importance", 5));
        item.source = "llm_extract";
        item.created_at = now;
        item.updated_at = now;

        if (!item.content.empty()) {
            memories.push_back(std::move(item));
        }
    }

    return memories;
}

std::string ContextMemoryManager::format_messages_for_prompt(
    const std::vector<ContextMemoryMessage>& messages) const {
    std::ostringstream stream;
    std::size_t used_tokens = 0;
    bool omitted = false;

    for (const auto& message : messages) {
        const std::string line =
            "[" + message.role + "] " + message.content + "\n";
        const std::size_t line_tokens = estimate_tokens(line);
        if (used_tokens + line_tokens > config_.compression_source_budget_tokens) {
            omitted = true;
            break;
        }

        stream << line;
        used_tokens += line_tokens;
    }

    if (omitted) {
        stream << "[system] 因摘要输入预算限制，后续部分历史消息已省略。\n";
    }

    return stream.str();
}

std::string ContextMemoryManager::format_long_term_memories(
    const MemoryState& state) const {
    std::vector<LongTermMemoryItem> memories = state.long_term_memories;
    std::sort(memories.begin(), memories.end(),
        [](const LongTermMemoryItem& left, const LongTermMemoryItem& right) {
            if (left.importance != right.importance) {
                return left.importance > right.importance;
            }
            if (left.hit_count != right.hit_count) {
                return left.hit_count > right.hit_count;
            }
            return left.updated_at > right.updated_at;
        });

    std::ostringstream stream;
    std::size_t used_tokens = 0;
    const std::size_t budget = long_term_budget();

    for (const auto& item : memories) {
        std::string line = "- ";
        if (!item.category.empty()) {
            line += "[" + item.category + "] ";
        }
        line += item.content;
        line += " (importance=" + std::to_string(item.importance) + ")\n";

        const std::size_t line_tokens = estimate_tokens(line);
        if (budget > 0 && used_tokens + line_tokens > budget) {
            break;
        }

        stream << line;
        used_tokens += line_tokens;
    }

    return trim(stream.str());
}

void ContextMemoryManager::merge_long_term_memories(
    MemoryState& state,
    const std::vector<LongTermMemoryItem>& memories) const {
    std::unordered_set<std::string> seen;
    for (const auto& item : state.long_term_memories) {
        seen.insert(normalize_for_dedup(item.content));
    }

    for (const auto& item : memories) {
        const std::string key = normalize_for_dedup(item.content);
        if (key.empty()) {
            continue;
        }

        auto existing = std::find_if(
            state.long_term_memories.begin(),
            state.long_term_memories.end(),
            [&](const LongTermMemoryItem& current) {
                return normalize_for_dedup(current.content) == key;
            });

        if (existing != state.long_term_memories.end()) {
            existing->importance =
                std::max(existing->importance, item.importance);
            existing->hit_count += 1;
            existing->updated_at = current_time_iso();
            continue;
        }

        if (seen.insert(key).second) {
            state.long_term_memories.push_back(item);
        }
    }

    std::sort(state.long_term_memories.begin(),
              state.long_term_memories.end(),
        [](const LongTermMemoryItem& left, const LongTermMemoryItem& right) {
            if (left.importance != right.importance) {
                return left.importance > right.importance;
            }
            return left.updated_at > right.updated_at;
        });

    if (state.long_term_memories.size() > config_.max_long_term_items) {
        state.long_term_memories.resize(config_.max_long_term_items);
    }
}

}  // namespace orchestrator
}  // namespace agent_rpc
