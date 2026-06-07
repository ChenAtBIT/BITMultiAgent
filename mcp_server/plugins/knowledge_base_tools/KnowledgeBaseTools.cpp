/**
 * @file KnowledgeBaseTools.cpp
 * @brief MCP 知识库工具插件 - 提供 sqlite-vec 文档向量化入库与相似度检索能力
 */

#include "PluginAPI.h"
#include "json.hpp"
#include "sqlite3.h"
#include "sqlite-vec.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using json = nlohmann::json;

namespace {

constexpr int kDefaultChunkSizeChars = 900;
constexpr int kDefaultChunkOverlapChars = 150;
constexpr int kDefaultTopK = 4;
constexpr int kDefaultExcerptChars = 420;
constexpr int kDefaultEmbeddingBatchSize = 12;
constexpr int kDefaultTimeoutMs = 15000;
constexpr int kDefaultRetryCount = 3;
constexpr int kDefaultRetryDelayMs = 1000;

/**
 * @brief 知识库工具定义
 */
static PluginTool tools[] = {
    {
        "ingest_knowledge_file",
        "读取本地文档，自动切片、向量化，并写入 sqlite-vec 知识库，适合文档入库、知识库构建和后续问答准备。",
        R"({
            "type": "object",
            "properties": {
                "path": {
                    "type": "string",
                    "description": "需要向量化并写入知识库的本地文件路径。"
                },
                "collection": {
                    "type": "string",
                    "description": "知识库集合名称，默认 default。"
                },
                "db_path": {
                    "type": "string",
                    "description": "sqlite-vec 数据库文件路径；未提供时优先读取环境变量 KNOWLEDGE_BASE_DB_PATH。"
                },
                "chunk_size_chars": {
                    "type": "integer",
                    "minimum": 200,
                    "maximum": 4000,
                    "description": "单个切片的目标字符数，默认 900。"
                },
                "chunk_overlap_chars": {
                    "type": "integer",
                    "minimum": 0,
                    "maximum": 1000,
                    "description": "相邻切片的重叠字符数，默认 150。"
                },
                "replace_existing": {
                    "type": "boolean",
                    "description": "是否替换同 collection + source_path 的旧切片，默认 true。"
                }
            },
            "required": ["path"]
        })"
    },
    {
        "search_knowledge_base",
        "根据用户问题向量检索 sqlite-vec 中最相近的知识片段，适合知识库问答、文档检索和基于资料的回答。",
        R"({
            "type": "object",
            "properties": {
                "query": {
                    "type": "string",
                    "description": "要检索的自然语言问题。"
                },
                "collection": {
                    "type": "string",
                    "description": "可选集合名称；为空时搜索整个知识库。"
                },
                "source_path": {
                    "type": "string",
                    "description": "可选源文件路径过滤条件。"
                },
                "db_path": {
                    "type": "string",
                    "description": "sqlite-vec 数据库文件路径；未提供时优先读取环境变量 KNOWLEDGE_BASE_DB_PATH。"
                },
                "top_k": {
                    "type": "integer",
                    "minimum": 1,
                    "maximum": 10,
                    "description": "返回最相近片段数量，默认 4。"
                },
                "max_excerpt_chars": {
                    "type": "integer",
                    "minimum": 80,
                    "maximum": 1200,
                    "description": "每个检索片段在返回结果中的最大字符数，默认 420。"
                }
            },
            "required": ["query"]
        })"
    }
};

/**
 * @brief 检索结果结构
 */
struct SearchMatch {
    std::string chunk_id;
    std::string collection;
    std::string source_path;
    std::string source_name;
    int chunk_index = 0;
    std::string chunk_text;
    double distance = 0.0;
};

/**
 * @brief 在堆上构造插件响应字符串
 * @param payload 要返回的 JSON 对象
 * @return 供宿主释放的 C 字符串
 */
char* make_response(const json& payload) {
    const std::string result = payload.dump();
    char* buffer = new char[result.size() + 1];
#ifdef _WIN32
    strcpy_s(buffer, result.size() + 1, result.c_str());
#else
    std::strcpy(buffer, result.c_str());
#endif
    return buffer;
}

/**
 * @brief 统一返回文本结果
 * @param text 结果文本
 * @param is_error 是否表示错误
 * @return MCP 结果 JSON
 */
json make_text_result(const std::string& text, bool is_error) {
    json response;
    response["content"] = json::array();
    response["content"].push_back({
        {"type", "text"},
        {"text", text}
    });
    response["isError"] = is_error;
    return response;
}

/**
 * @brief 去掉字符串首尾空白
 * @param text 原始字符串
 * @return 去空白后的结果
 */
std::string trim(const std::string& text) {
    size_t start = 0;
    while (start < text.size() &&
           std::isspace(static_cast<unsigned char>(text[start]))) {
        start++;
    }

    size_t end = text.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        end--;
    }

    return text.substr(start, end - start);
}

/**
 * @brief 读取字符串参数
 * @param args 参数对象
 * @param key 字段名
 * @param default_value 默认值
 * @return 解析后的字符串
 */
std::string get_string_arg(const json& args,
                           const std::string& key,
                           const std::string& default_value) {
    if (!args.contains(key) || args[key].is_null()) {
        return default_value;
    }
    if (args[key].is_string()) {
        return args[key].get<std::string>();
    }
    return default_value;
}

/**
 * @brief 读取整数参数
 * @param args 参数对象
 * @param key 字段名
 * @param default_value 默认值
 * @return 解析后的整数
 */
int get_int_arg(const json& args, const std::string& key, int default_value) {
    if (!args.contains(key) || args[key].is_null()) {
        return default_value;
    }
    if (args[key].is_number_integer()) {
        return args[key].get<int>();
    }
    if (args[key].is_number()) {
        return static_cast<int>(args[key].get<double>());
    }
    return default_value;
}

/**
 * @brief 读取布尔参数
 * @param args 参数对象
 * @param key 字段名
 * @param default_value 默认值
 * @return 解析后的布尔值
 */
bool get_bool_arg(const json& args, const std::string& key, bool default_value) {
    if (!args.contains(key) || args[key].is_null()) {
        return default_value;
    }
    if (args[key].is_boolean()) {
        return args[key].get<bool>();
    }
    return default_value;
}

/**
 * @brief 读取环境变量
 * @param name 环境变量名
 * @return 环境变量值；不存在时返回空字符串
 */
std::string get_env_or_empty(const char* name) {
    const char* value = std::getenv(name);
    if (!value || value[0] == '\0') {
        return "";
    }
    return value;
}

/**
 * @brief 获取默认知识库数据库路径
 * @return 默认数据库文件路径
 */
std::string default_db_path() {
    const std::string env_path = get_env_or_empty("KNOWLEDGE_BASE_DB_PATH");
    if (!env_path.empty()) {
        return env_path;
    }
    return "knowledge_base.db";
}

/**
 * @brief 规范化集合名
 * @param collection 原始集合名
 * @return 清洗后的集合名
 */
std::string normalize_collection_name(const std::string& collection) {
    const std::string trimmed = trim(collection);
    if (trimmed.empty()) {
        return "default";
    }
    return trimmed;
}

/**
 * @brief 将输入路径规范化为绝对路径
 * @param raw_path 用户传入路径
 * @return 规范化后的路径
 */
std::filesystem::path normalize_path(const std::string& raw_path) {
    if (raw_path.empty()) {
        throw std::runtime_error("文件路径不能为空");
    }

    const std::filesystem::path path = raw_path;
    if (path.is_absolute()) {
        return path.lexically_normal();
    }

    return std::filesystem::absolute(path).lexically_normal();
}

/**
 * @brief 规范化数据库路径
 * @param raw_path 原始路径
 * @return 规范化后的数据库路径
 */
std::filesystem::path normalize_db_path(const std::string& raw_path) {
    return normalize_path(raw_path.empty() ? default_db_path() : raw_path);
}

/**
 * @brief 检查文件是否疑似二进制内容
 * @param path 文件路径
 * @return true 表示检测到 NUL 字节
 */
bool looks_like_binary_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        throw std::runtime_error("无法打开文件: " + path.string());
    }

    std::vector<char> buffer(4096, '\0');
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize bytes_read = input.gcount();

    for (std::streamsize index = 0; index < bytes_read; ++index) {
        if (buffer[static_cast<size_t>(index)] == '\0') {
            return true;
        }
    }
    return false;
}

/**
 * @brief 读取完整文本文件
 * @param path 文件路径
 * @return 文件内容
 */
std::string read_text_file(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        throw std::runtime_error("文件不存在: " + path.string());
    }
    if (!std::filesystem::is_regular_file(path)) {
        throw std::runtime_error("目标不是普通文件: " + path.string());
    }
    if (looks_like_binary_file(path)) {
        throw std::runtime_error("当前知识库工具只支持文本文件，检测到二进制内容: " + path.string());
    }

    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        throw std::runtime_error("无法读取文件: " + path.string());
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

/**
 * @brief 将文本中的换行统一为 \n
 * @param text 原始文本
 * @return 统一换行后的文本
 */
std::string normalize_newlines(const std::string& text) {
    std::string normalized;
    normalized.reserve(text.size());

    for (size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '\r') {
            if (index + 1 < text.size() && text[index + 1] == '\n') {
                continue;
            }
            normalized.push_back('\n');
            continue;
        }
        normalized.push_back(text[index]);
    }

    return normalized;
}

/**
 * @brief 将偏移回退到 UTF-8 安全边界
 * @param text 原始文本
 * @param offset 目标偏移
 * @return 可安全切分的偏移
 */
size_t utf8_safe_boundary_before(const std::string& text, size_t offset) {
    size_t safe_offset = std::min(offset, text.size());
    while (safe_offset > 0 &&
           safe_offset < text.size() &&
           (static_cast<unsigned char>(text[safe_offset]) & 0xC0) == 0x80) {
        safe_offset--;
    }
    return safe_offset;
}

/**
 * @brief 截断文本片段，避免工具返回过长上下文
 * @param text 原始文本
 * @param max_chars 最大字符数
 * @return 截断后的片段
 */
std::string make_excerpt(const std::string& text, size_t max_chars) {
    std::string excerpt = trim(text);
    if (excerpt.size() <= max_chars) {
        return excerpt;
    }

    const size_t safe_end = utf8_safe_boundary_before(excerpt, max_chars);
    excerpt.resize(safe_end == 0 ? max_chars : safe_end);
    excerpt = trim(excerpt);
    excerpt += "...";
    return excerpt;
}

/**
 * @brief 计算简单稳定的 FNV-1a 摘要
 * @param text 输入文本
 * @return 16 进制字符串
 */
std::string fnv1a_hex(const std::string& text) {
    constexpr std::uint64_t kOffsetBasis = 1469598103934665603ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;

    std::uint64_t value = kOffsetBasis;
    for (unsigned char ch : text) {
        value ^= static_cast<std::uint64_t>(ch);
        value *= kPrime;
    }

    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << value;
    return oss.str();
}

/**
 * @brief 将浮点向量序列化为 JSON 数组文本，供 sqlite-vec 直接写入
 * @param embedding 向量数据
 * @return JSON 文本
 */
std::string embedding_to_json(const std::vector<float>& embedding) {
    json array = json::array();
    for (float value : embedding) {
        array.push_back(value);
    }
    return array.dump();
}

/**
 * @brief 把长文档切分为可向量化的小块
 * @param text 原始文本
 * @param chunk_size_chars 目标块大小
 * @param overlap_chars 相邻块重叠字符数
 * @return 切片列表
 */
std::vector<std::string> split_text_into_chunks(const std::string& text,
                                                int chunk_size_chars,
                                                int overlap_chars) {
    const std::string normalized = normalize_newlines(text);
    const std::string trimmed = trim(normalized);
    if (trimmed.empty()) {
        return {};
    }

    const size_t max_chars = static_cast<size_t>(std::clamp(chunk_size_chars, 200, 4000));
    const size_t overlap = static_cast<size_t>(std::clamp(overlap_chars, 0, 1000));

    if (trimmed.size() <= max_chars) {
        return {trimmed};
    }

    std::vector<std::string> chunks;
    size_t start = 0;

    while (start < trimmed.size()) {
        size_t candidate_end = std::min(start + max_chars, trimmed.size());
        candidate_end = utf8_safe_boundary_before(trimmed, candidate_end);
        if (candidate_end <= start) {
            candidate_end = std::min(start + max_chars, trimmed.size());
        }
        size_t end = candidate_end;

        if (candidate_end < trimmed.size()) {
            // 优先在段落、换行和空格处切分，避免语义被粗暴截断。
            size_t paragraph_break = trimmed.rfind("\n\n", candidate_end);
            if (paragraph_break != std::string::npos &&
                paragraph_break > start + max_chars / 2) {
                end = paragraph_break + 2;
            } else {
                size_t line_break = trimmed.rfind('\n', candidate_end);
                if (line_break != std::string::npos &&
                    line_break > start + max_chars / 2) {
                    end = line_break + 1;
                } else {
                    size_t space_break = trimmed.rfind(' ', candidate_end);
                    if (space_break != std::string::npos &&
                        space_break > start + max_chars / 2) {
                        end = space_break + 1;
                    }
                }
            }
        }

        if (end <= start) {
            end = candidate_end;
        }

        end = utf8_safe_boundary_before(trimmed, end);
        if (end <= start) {
            end = candidate_end;
        }

        const std::string chunk = trim(trimmed.substr(start, end - start));
        if (!chunk.empty()) {
            chunks.push_back(chunk);
        }

        if (end >= trimmed.size()) {
            break;
        }

        size_t next_start = end;
        if (overlap > 0 && end > overlap) {
            next_start = end - overlap;
        }

        next_start = utf8_safe_boundary_before(trimmed, next_start);
        if (next_start <= start) {
            next_start = end;
        }

        // 为了避免重叠区域从词中间开始，尽量前移到空白字符边界。
        while (next_start > start &&
               next_start < trimmed.size() &&
               !std::isspace(static_cast<unsigned char>(trimmed[next_start - 1])) &&
               next_start > end - overlap / 2) {
            next_start--;
        }

        if (next_start <= start) {
            next_start = end;
        }
        start = next_start;
    }

    return chunks;
}

/**
 * @brief CURL 写回调
 * @param contents 数据指针
 * @param size 单个元素大小
 * @param nmemb 元素数量
 * @param userp 输出缓冲区
 * @return 实际写入字节数
 */
size_t curl_write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* output = static_cast<std::string*>(userp);
    output->append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

/**
 * @brief DashScope Embedding 客户端
 */
class DashScopeEmbeddingClient {
public:
    /**
     * @brief 构造 Embedding 客户端
     * @param api_key API Key
     * @param model 模型名
     */
    DashScopeEmbeddingClient(std::string api_key, std::string model)
        : api_key_(std::move(api_key))
        , model_(std::move(model)) {
        if (api_key_.empty()) {
            api_key_ = get_env_or_empty("DASHSCOPE_API_KEY");
        }
        if (model_.empty()) {
            model_ = get_env_or_empty("KNOWLEDGE_BASE_EMBEDDING_MODEL");
        }
        if (model_.empty()) {
            model_ = "text-embedding-v2";
        }
        if (api_key_.empty()) {
            throw std::runtime_error("未找到 DASHSCOPE_API_KEY，无法执行文档向量化或检索");
        }
    }

    /**
     * @brief 批量生成文本向量
     * @param texts 待向量化文本
     * @param text_type document 或 query
     * @return 向量列表
     */
    std::vector<std::vector<float>> embed_batch(const std::vector<std::string>& texts,
                                                const std::string& text_type) const {
        std::vector<std::vector<float>> all_embeddings;
        if (texts.empty()) {
            return all_embeddings;
        }

        for (size_t offset = 0; offset < texts.size(); offset += kDefaultEmbeddingBatchSize) {
            const size_t end = std::min(offset + static_cast<size_t>(kDefaultEmbeddingBatchSize),
                                        texts.size());
            std::vector<std::string> batch;
            batch.reserve(end - offset);

            for (size_t index = offset; index < end; ++index) {
                const std::string sanitized = texts[index].empty() ? " " : texts[index];
                batch.push_back(sanitized);
            }

            const json request_body = {
                {"model", model_},
                {"input", {
                    {"texts", batch}
                }},
                {"parameters", {
                    {"text_type", text_type}
                }}
            };

            const std::string response = call_api_with_retry(request_body.dump());
            auto embeddings = parse_embeddings(response);
            if (embeddings.size() != batch.size()) {
                throw std::runtime_error("DashScope 返回的 embedding 数量与请求不一致");
            }

            all_embeddings.insert(all_embeddings.end(),
                                  embeddings.begin(),
                                  embeddings.end());
        }

        return all_embeddings;
    }

    /**
     * @brief 生成查询向量
     * @param query 查询文本
     * @return 单条向量
     */
    std::vector<float> embed_query(const std::string& query) const {
        auto embeddings = embed_batch({query}, "query");
        if (embeddings.empty()) {
            throw std::runtime_error("查询向量化结果为空");
        }
        return embeddings.front();
    }

    /**
     * @brief 获取实际使用的模型名
     * @return 模型名
     */
    const std::string& model() const {
        return model_;
    }

private:
    /**
     * @brief 发送 HTTP POST 请求
     * @param request_body 请求体
     * @return 原始响应
     */
    std::string send_post_request(const std::string& request_body) const {
        CURL* curl = curl_easy_init();
        if (!curl) {
            throw std::runtime_error("无法初始化 CURL");
        }

        std::string response;
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        const std::string auth_header = "Authorization: Bearer " + api_key_;
        headers = curl_slist_append(headers, auth_header.c_str());

        curl_easy_setopt(curl, CURLOPT_URL,
                         "https://dashscope.aliyuncs.com/api/v1/services/embeddings/text-embedding/text-embedding");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kDefaultTimeoutMs);

        const CURLcode result = curl_easy_perform(curl);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (result != CURLE_OK) {
            throw std::runtime_error(std::string("DashScope 请求失败: ") +
                                     curl_easy_strerror(result));
        }

        return response;
    }

    /**
     * @brief 带重试的 API 调用
     * @param request_body 请求体
     * @return 原始响应
     */
    std::string call_api_with_retry(const std::string& request_body) const {
        std::string last_error;

        for (int attempt = 0; attempt <= kDefaultRetryCount; ++attempt) {
            if (attempt > 0) {
                const int delay_ms = kDefaultRetryDelayMs * (1 << (attempt - 1));
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            }

            try {
                return send_post_request(request_body);
            } catch (const std::exception& ex) {
                last_error = ex.what();
            }
        }

        throw std::runtime_error("DashScope 向量化失败: " + last_error);
    }

    /**
     * @brief 解析 DashScope 响应
     * @param response 原始响应字符串
     * @return 向量列表
     */
    static std::vector<std::vector<float>> parse_embeddings(const std::string& response) {
        json response_json = json::parse(response);
        if (response_json.contains("code")) {
            throw std::runtime_error(
                "DashScope 返回错误: " +
                response_json.value("message", std::string("unknown error")));
        }

        if (!response_json.contains("output") ||
            !response_json["output"].contains("embeddings")) {
            throw std::runtime_error("DashScope 响应缺少 embeddings 字段");
        }

        std::vector<std::vector<float>> embeddings;
        for (const auto& item : response_json["output"]["embeddings"]) {
            if (!item.contains("embedding")) {
                continue;
            }

            std::vector<float> values;
            for (const auto& value : item["embedding"]) {
                values.push_back(value.get<float>());
            }
            embeddings.push_back(std::move(values));
        }

        if (embeddings.empty()) {
            throw std::runtime_error("DashScope 未返回有效 embedding");
        }

        return embeddings;
    }

    std::string api_key_;
    std::string model_;
};

/**
 * @brief sqlite-vec 知识库存储封装
 */
class SQLiteVecKnowledgeBase {
public:
    /**
     * @brief 构造知识库存储
     * @param db_path 数据库路径
     */
    explicit SQLiteVecKnowledgeBase(const std::filesystem::path& db_path)
        : db_path_(db_path) {
    }

    /**
     * @brief 析构时关闭数据库连接
     */
    ~SQLiteVecKnowledgeBase() {
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
    }

    /**
     * @brief 打开数据库并初始化 sqlite-vec
     */
    void open() {
        if (db_) {
            return;
        }

        if (db_path_.has_parent_path()) {
            std::filesystem::create_directories(db_path_.parent_path());
        }

        const int rc = sqlite3_open_v2(
            db_path_.string().c_str(),
            &db_,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
            nullptr);
        if (rc != SQLITE_OK || !db_) {
            const std::string error = db_ ? sqlite3_errmsg(db_) : "无法创建 sqlite3 连接";
            throw std::runtime_error("打开知识库数据库失败: " + error);
        }

        exec_sql("PRAGMA journal_mode=WAL;");
        exec_sql("PRAGMA synchronous=NORMAL;");
        exec_sql("PRAGMA busy_timeout=3000;");
        initialize_vec_extension();
        ensure_info_table();
    }

    /**
     * @brief 根据 embedding 维度和模型初始化表结构
     * @param dimension embedding 维度
     * @param model embedding 模型名
     */
    void ensure_schema(int dimension, const std::string& model) {
        open();

        const std::optional<std::string> stored_dimension = get_info("embedding_dimension");
        const std::optional<std::string> stored_model = get_info("embedding_model");

        if (stored_dimension.has_value() &&
            std::stoi(stored_dimension.value()) != dimension) {
            throw std::runtime_error(
                "当前知识库已使用维度 " + stored_dimension.value() +
                " 建表，无法再写入维度 " + std::to_string(dimension) +
                " 的向量。请更换 db_path 或清理旧库后重试。");
        }

        if (stored_model.has_value() && stored_model.value() != model) {
            throw std::runtime_error(
                "当前知识库已使用模型 " + stored_model.value() +
                " 建库，不能混用模型 " + model +
                "。请更换 db_path 或使用统一 embedding 模型。");
        }

        if (!stored_dimension.has_value()) {
            const std::string create_sql =
                "CREATE VIRTUAL TABLE IF NOT EXISTS kb_chunks USING vec0("
                "chunk_id text primary key,"
                "collection text,"
                "source_path text,"
                "source_name text,"
                "chunk_index integer,"
                "contents_embedding float[" + std::to_string(dimension) + "],"
                "+chunk_text text,"
                "+checksum text"
                ");";
            exec_sql(create_sql);
            set_info("embedding_dimension", std::to_string(dimension));
            set_info("embedding_model", model);
        }
    }

    /**
     * @brief 清理某个文档在指定集合中的旧切片
     * @param collection 集合名
     * @param source_path 源文件路径
     * @return 被删除的旧切片数量
     */
    int clear_source(const std::string& collection, const std::string& source_path) {
        open();

        sqlite3_stmt* stmt = nullptr;
        prepare_statement(
            "SELECT chunk_id FROM kb_chunks WHERE collection = ? AND source_path = ?;",
            &stmt);
        bind_text(stmt, 1, collection);
        bind_text(stmt, 2, source_path);

        std::vector<std::string> chunk_ids;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* value = sqlite3_column_text(stmt, 0);
            if (value) {
                chunk_ids.emplace_back(reinterpret_cast<const char*>(value));
            }
        }
        finalize_statement(stmt);

        sqlite3_stmt* delete_stmt = nullptr;
        prepare_statement("DELETE FROM kb_chunks WHERE chunk_id = ?;", &delete_stmt);
        for (const auto& chunk_id : chunk_ids) {
            sqlite3_reset(delete_stmt);
            sqlite3_clear_bindings(delete_stmt);
            bind_text(delete_stmt, 1, chunk_id);
            step_done(delete_stmt, "删除旧知识块");
        }
        finalize_statement(delete_stmt);

        return static_cast<int>(chunk_ids.size());
    }

    /**
     * @brief 写入知识块
     * @param collection 集合名
     * @param source_path 源文件路径
     * @param source_name 源文件展示名
     * @param checksum 源文件摘要
     * @param chunks 文本切片
     * @param embeddings 对应向量
     */
    void ingest_chunks(const std::string& collection,
                       const std::string& source_path,
                       const std::string& source_name,
                       const std::string& checksum,
                       const std::vector<std::string>& chunks,
                       const std::vector<std::vector<float>>& embeddings) {
        open();

        if (chunks.size() != embeddings.size()) {
            throw std::runtime_error("知识块数量和 embedding 数量不一致");
        }

        sqlite3_stmt* stmt = nullptr;
        prepare_statement(
            "INSERT OR REPLACE INTO kb_chunks("
            "chunk_id, collection, source_path, source_name, chunk_index, "
            "contents_embedding, chunk_text, checksum"
            ") VALUES (?, ?, ?, ?, ?, ?, ?, ?);",
            &stmt);

        for (size_t index = 0; index < chunks.size(); ++index) {
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);

            const std::string chunk_id =
                collection + "::" + source_path + "::" + std::to_string(index);

            bind_text(stmt, 1, chunk_id);
            bind_text(stmt, 2, collection);
            bind_text(stmt, 3, source_path);
            bind_text(stmt, 4, source_name);
            bind_int(stmt, 5, static_cast<int>(index));
            const std::string embedding_json = embedding_to_json(embeddings[index]);
            bind_text(stmt, 6, embedding_json);
            bind_text(stmt, 7, chunks[index]);
            bind_text(stmt, 8, checksum);

            step_done(stmt, "写入知识块");
        }

        finalize_statement(stmt);
    }

    /**
     * @brief 在知识库中执行相似度检索
     * @param query_embedding 查询向量
     * @param collection 集合过滤
     * @param source_path 源文件过滤
     * @param top_k 返回数量
     * @param max_excerpt_chars 片段返回上限
     * @return 检索结果
     */
    std::vector<SearchMatch> search(const std::vector<float>& query_embedding,
                                    const std::string& collection,
                                    const std::string& source_path,
                                    int top_k,
                                    int max_excerpt_chars) {
        open();

        sqlite3_stmt* stmt = nullptr;
        const std::string embedding_json = embedding_to_json(query_embedding);
        const int safe_top_k = std::clamp(top_k, 1, 10);
        const int safe_excerpt_chars = std::clamp(max_excerpt_chars, 80, 1200);

        std::string sql =
            "SELECT chunk_id, collection, source_path, source_name, chunk_index, chunk_text, distance "
            "FROM kb_chunks "
            "WHERE contents_embedding MATCH ? AND k = ?";

        const bool has_collection = !trim(collection).empty();
        const bool has_source_path = !trim(source_path).empty();
        if (has_collection) {
            sql += " AND collection = ?";
        }
        if (has_source_path) {
            sql += " AND source_path = ?";
        }
        sql += " ORDER BY distance LIMIT ?;";

        prepare_statement(sql, &stmt);

        int bind_index = 1;
        bind_text(stmt, bind_index++, embedding_json);
        bind_int(stmt, bind_index++, safe_top_k);
        if (has_collection) {
            bind_text(stmt, bind_index++, collection);
        }
        if (has_source_path) {
            bind_text(stmt, bind_index++, source_path);
        }
        bind_int(stmt, bind_index, safe_top_k);

        std::vector<SearchMatch> matches;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            SearchMatch match;
            match.chunk_id = column_text(stmt, 0);
            match.collection = column_text(stmt, 1);
            match.source_path = column_text(stmt, 2);
            match.source_name = column_text(stmt, 3);
            match.chunk_index = sqlite3_column_int(stmt, 4);
            match.chunk_text = make_excerpt(column_text(stmt, 5),
                                            static_cast<size_t>(safe_excerpt_chars));
            match.distance = sqlite3_column_double(stmt, 6);
            matches.push_back(std::move(match));
        }

        finalize_statement(stmt);
        return matches;
    }

    /**
     * @brief 获取数据库路径
     * @return 数据库路径字符串
     */
    std::string db_path() const {
        return db_path_.string();
    }

private:
    /**
     * @brief 初始化 sqlite-vec 扩展
     */
    void initialize_vec_extension() {
        char* error_message = nullptr;
        const int rc = sqlite3_vec_init(db_, &error_message, nullptr);
        if (rc != SQLITE_OK) {
            std::string error = error_message ? error_message : "unknown error";
            if (error_message) {
                sqlite3_free(error_message);
            }
            throw std::runtime_error("初始化 sqlite-vec 失败: " + error);
        }
    }

    /**
     * @brief 确保元信息表存在
     */
    void ensure_info_table() {
        exec_sql(
            "CREATE TABLE IF NOT EXISTS kb_info ("
            "key TEXT PRIMARY KEY,"
            "value TEXT NOT NULL"
            ");");
    }

    /**
     * @brief 查询知识库元信息
     * @param key 元信息键
     * @return 查询结果
     */
    std::optional<std::string> get_info(const std::string& key) {
        sqlite3_stmt* stmt = nullptr;
        prepare_statement("SELECT value FROM kb_info WHERE key = ?;", &stmt);
        bind_text(stmt, 1, key);

        std::optional<std::string> result;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            result = column_text(stmt, 0);
        }

        finalize_statement(stmt);
        return result;
    }

    /**
     * @brief 写入知识库元信息
     * @param key 元信息键
     * @param value 元信息值
     */
    void set_info(const std::string& key, const std::string& value) {
        sqlite3_stmt* stmt = nullptr;
        prepare_statement(
            "INSERT INTO kb_info(key, value) VALUES (?, ?) "
            "ON CONFLICT(key) DO UPDATE SET value = excluded.value;",
            &stmt);
        bind_text(stmt, 1, key);
        bind_text(stmt, 2, value);
        step_done(stmt, "写入知识库元信息");
        finalize_statement(stmt);
    }

    /**
     * @brief 执行无结果 SQL
     * @param sql SQL 语句
     */
    void exec_sql(const std::string& sql) {
        char* error_message = nullptr;
        const int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error_message);
        if (rc != SQLITE_OK) {
            std::string error = error_message ? error_message : "unknown error";
            if (error_message) {
                sqlite3_free(error_message);
            }
            throw std::runtime_error("执行 SQL 失败: " + error + " | SQL=" + sql);
        }
    }

    /**
     * @brief 预编译 SQL 语句
     * @param sql SQL 文本
     * @param stmt 输出语句句柄
     */
    void prepare_statement(const std::string& sql, sqlite3_stmt** stmt) {
        const int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, stmt, nullptr);
        if (rc != SQLITE_OK) {
            throw std::runtime_error("预编译 SQL 失败: " + std::string(sqlite3_errmsg(db_)));
        }
    }

    /**
     * @brief 结束语句对象
     * @param stmt 语句句柄
     */
    static void finalize_statement(sqlite3_stmt* stmt) {
        if (stmt) {
            sqlite3_finalize(stmt);
        }
    }

    /**
     * @brief 校验执行结果是否为 DONE
     * @param stmt 语句句柄
     * @param context 错误上下文
     */
    void step_done(sqlite3_stmt* stmt, const std::string& context) {
        const int rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            throw std::runtime_error(context + "失败: " + sqlite3_errmsg(db_));
        }
    }

    /**
     * @brief 绑定文本参数
     * @param stmt 语句句柄
     * @param index 参数位置
     * @param value 参数值
     */
    static void bind_text(sqlite3_stmt* stmt, int index, const std::string& value) {
        sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT);
    }

    /**
     * @brief 绑定整数参数
     * @param stmt 语句句柄
     * @param index 参数位置
     * @param value 参数值
     */
    static void bind_int(sqlite3_stmt* stmt, int index, int value) {
        sqlite3_bind_int(stmt, index, value);
    }

    /**
     * @brief 读取文本列
     * @param stmt 语句句柄
     * @param index 列下标
     * @return 文本值
     */
    static std::string column_text(sqlite3_stmt* stmt, int index) {
        const unsigned char* value = sqlite3_column_text(stmt, index);
        return value ? reinterpret_cast<const char*>(value) : "";
    }

    std::filesystem::path db_path_;
    sqlite3* db_ = nullptr;
};

/**
 * @brief 处理文件向量化入库
 * @param args 工具参数
 * @return JSON 文本结果
 */
std::string handle_ingest_knowledge_file(const json& args) {
    const std::filesystem::path source_path = normalize_path(
        get_string_arg(args, "path", ""));
    const std::string collection = normalize_collection_name(
        get_string_arg(args, "collection", ""));
    const std::filesystem::path db_path = normalize_db_path(
        get_string_arg(args, "db_path", ""));
    const int chunk_size_chars = get_int_arg(
        args, "chunk_size_chars", kDefaultChunkSizeChars);
    const int chunk_overlap_chars = get_int_arg(
        args, "chunk_overlap_chars", kDefaultChunkOverlapChars);
    const bool replace_existing = get_bool_arg(args, "replace_existing", true);

    const std::string raw_text = read_text_file(source_path);
    const std::vector<std::string> chunks = split_text_into_chunks(
        raw_text, chunk_size_chars, chunk_overlap_chars);
    if (chunks.empty()) {
        throw std::runtime_error("文件内容为空，无法建立知识库索引: " + source_path.string());
    }

    const std::string api_key = get_string_arg(
        args, "dashscope_api_key", get_env_or_empty("DASHSCOPE_API_KEY"));
    const std::string model = get_string_arg(
        args, "embedding_model", get_env_or_empty("KNOWLEDGE_BASE_EMBEDDING_MODEL"));
    DashScopeEmbeddingClient embedding_client(api_key, model);
    const std::vector<std::vector<float>> embeddings =
        embedding_client.embed_batch(chunks, "document");

    SQLiteVecKnowledgeBase knowledge_base(db_path);
    knowledge_base.ensure_schema(static_cast<int>(embeddings.front().size()),
                                 embedding_client.model());

    int replaced_count = 0;
    if (replace_existing) {
        replaced_count = knowledge_base.clear_source(collection, source_path.string());
    }

    const std::string checksum = fnv1a_hex(raw_text);
    knowledge_base.ingest_chunks(collection,
                                 source_path.string(),
                                 source_path.filename().string(),
                                 checksum,
                                 chunks,
                                 embeddings);

    json result;
    result["success"] = true;
    result["action"] = "ingest";
    result["collection"] = collection;
    result["source_path"] = source_path.string();
    result["db_path"] = knowledge_base.db_path();
    result["embedding_model"] = embedding_client.model();
    result["embedding_dimension"] = embeddings.front().size();
    result["chunk_count"] = chunks.size();
    result["replaced_chunk_count"] = replaced_count;
    result["checksum"] = checksum;
    result["chunk_size_chars"] = std::clamp(chunk_size_chars, 200, 4000);
    result["chunk_overlap_chars"] = std::clamp(chunk_overlap_chars, 0, 1000);
    result["preview"] = make_excerpt(chunks.front(), 160);
    return result.dump();
}

/**
 * @brief 处理知识库检索
 * @param args 工具参数
 * @return JSON 文本结果
 */
std::string handle_search_knowledge_base(const json& args) {
    const std::string query = trim(get_string_arg(args, "query", ""));
    if (query.empty()) {
        throw std::runtime_error("query 不能为空");
    }

    const std::string collection = normalize_collection_name(
        get_string_arg(args, "collection", ""));
    const std::string source_path_filter = trim(get_string_arg(args, "source_path", ""));
    const std::filesystem::path db_path = normalize_db_path(
        get_string_arg(args, "db_path", ""));
    const int top_k = get_int_arg(args, "top_k", kDefaultTopK);
    const int max_excerpt_chars = get_int_arg(
        args, "max_excerpt_chars", kDefaultExcerptChars);

    const std::string api_key = get_string_arg(
        args, "dashscope_api_key", get_env_or_empty("DASHSCOPE_API_KEY"));
    const std::string model = get_string_arg(
        args, "embedding_model", get_env_or_empty("KNOWLEDGE_BASE_EMBEDDING_MODEL"));
    DashScopeEmbeddingClient embedding_client(api_key, model);
    const std::vector<float> query_embedding = embedding_client.embed_query(query);

    SQLiteVecKnowledgeBase knowledge_base(db_path);
    knowledge_base.ensure_schema(static_cast<int>(query_embedding.size()),
                                 embedding_client.model());

    const std::vector<SearchMatch> matches = knowledge_base.search(
        query_embedding,
        collection == "default" ? "" : collection,
        source_path_filter.empty() ? "" : normalize_path(source_path_filter).string(),
        top_k,
        max_excerpt_chars);

    json result;
    result["success"] = true;
    result["action"] = "search";
    result["query"] = query;
    result["collection"] = collection == "default" ? "" : collection;
    result["source_path_filter"] = source_path_filter.empty()
        ? ""
        : normalize_path(source_path_filter).string();
    result["db_path"] = knowledge_base.db_path();
    result["embedding_model"] = embedding_client.model();
    result["embedding_dimension"] = query_embedding.size();
    result["match_count"] = matches.size();
    result["matches"] = json::array();

    for (const auto& match : matches) {
        result["matches"].push_back({
            {"chunk_id", match.chunk_id},
            {"collection", match.collection},
            {"source_path", match.source_path},
            {"source_name", match.source_name},
            {"chunk_index", match.chunk_index},
            {"distance", match.distance},
            {"content", match.chunk_text}
        });
    }

    return result.dump();
}

}  // namespace

const char* GetNameImpl() {
    return "knowledge-base-tools";
}

const char* GetVersionImpl() {
    return "1.0.0";
}

PluginType GetTypeImpl() {
    return PLUGIN_TYPE_TOOLS;
}

int InitializeImpl() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    return 1;
}

char* HandleRequestImpl(const char* req) {
    try {
        const json request = json::parse(req);
        const std::string tool_name = request.at("params").at("name").get<std::string>();
        const json arguments = request.at("params").value("arguments", json::object());

        if (tool_name == "ingest_knowledge_file") {
            return make_response(make_text_result(handle_ingest_knowledge_file(arguments), false));
        }
        if (tool_name == "search_knowledge_base") {
            return make_response(make_text_result(handle_search_knowledge_base(arguments), false));
        }

        return make_response(make_text_result("未知工具: " + tool_name, true));
    } catch (const std::exception& ex) {
        return make_response(make_text_result("错误: " + std::string(ex.what()), true));
    }
}

void ShutdownImpl() {
    curl_global_cleanup();
}

int GetToolCountImpl() {
    return static_cast<int>(sizeof(tools) / sizeof(tools[0]));
}

const PluginTool* GetToolImpl(int index) {
    if (index < 0 || index >= GetToolCountImpl()) {
        return nullptr;
    }
    return &tools[index];
}

static PluginAPI plugin = {
    GetNameImpl,
    GetVersionImpl,
    GetTypeImpl,
    InitializeImpl,
    HandleRequestImpl,
    ShutdownImpl,
    GetToolCountImpl,
    GetToolImpl,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr
};

extern "C" PLUGIN_API PluginAPI* CreatePlugin() {
    return &plugin;
}

extern "C" PLUGIN_API void DestroyPlugin(PluginAPI*) {
}
