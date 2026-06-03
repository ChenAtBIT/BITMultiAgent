#pragma once

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using json = nlohmann::json;

/**
 * @brief 通义千问工具定义
 */
struct QwenToolDefinition {
    std::string name;
    std::string description;
    std::string parameters_json;
};

/**
 * @brief 通义千问工具调用信息
 */
struct QwenToolCall {
    std::string id;
    std::string name;
    std::string arguments_json;
};

/**
 * @brief 通义千问消息结构
 */
struct QwenMessage {
    std::string role;
    std::string content;
    std::string name;
    std::string tool_call_id;
    std::vector<QwenToolCall> tool_calls;
};

/**
 * @brief 通义千问单轮响应结果
 */
struct QwenChatResult {
    std::string content;
    std::vector<QwenToolCall> tool_calls;
};

/**
 * @brief 阿里百炼 API 客户端
 * 用于调用通义千问模型，并支持原生 Function Calling
 */
class QwenClient {
public:
    explicit QwenClient(const std::string& api_key,
                       const std::string& model = "qwen-plus")
        : api_key_(api_key)
        , model_(model)
        , api_url_("https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions") {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }

    ~QwenClient() {
        curl_global_cleanup();
    }

    /**
     * @brief 发起普通对话请求
     * @param system_prompt 系统提示词
     * @param user_message 用户消息
     * @return AI 回复
     */
    std::string chat(const std::string& system_prompt,
                    const std::string& user_message) {
        std::vector<QwenMessage> messages = {
            QwenMessage{"system", system_prompt, "", "", {}},
            QwenMessage{"user", user_message, "", "", {}}
        };
        return chat_completion(messages).content;
    }

    /**
     * @brief 发起支持工具调用的对话请求
     * @param messages 完整消息历史
     * @param tools 可选工具定义
     * @return 单轮响应结果，可能包含 tool_calls
     */
    QwenChatResult chat_completion(const std::vector<QwenMessage>& messages,
                                   const std::vector<QwenToolDefinition>& tools = {}) {
        json request_body;
        request_body["model"] = model_;
        request_body["messages"] = build_messages_json(messages);

        if (!tools.empty()) {
            // 按 OpenAI 兼容模式传入 tools，让模型自己决定是否调用工具。
            request_body["tools"] = build_tools_json(tools);
            request_body["tool_choice"] = "auto";
        }

        const std::string response = send_post_request(request_body.dump());
        return parse_chat_result(response);
    }

private:
    /**
     * @brief libcurl 写回调
     * @param contents 当前返回的数据块
     * @param size 单个元素大小
     * @param nmemb 元素个数
     * @param userp 累积响应的缓冲区
     * @return 实际写入字节数
     */
    static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
        ((std::string*)userp)->append((char*)contents, size * nmemb);
        return size * nmemb;
    }

    /**
     * @brief 将消息列表序列化为兼容模式请求格式
     * @param messages 消息历史
     * @return JSON 数组
     */
    static json build_messages_json(const std::vector<QwenMessage>& messages) {
        json messages_json = json::array();

        for (const auto& message : messages) {
            json message_json = {
                {"role", message.role}
            };

            if (!message.name.empty() && message.role != "tool") {
                message_json["name"] = message.name;
            }
            if (!message.tool_call_id.empty()) {
                message_json["tool_call_id"] = message.tool_call_id;
            }

            if (!message.tool_calls.empty()) {
                json tool_calls_json = json::array();
                for (const auto& tool_call : message.tool_calls) {
                    tool_calls_json.push_back({
                        {"id", tool_call.id},
                        {"type", "function"},
                        {"function", {
                            {"name", tool_call.name},
                            {"arguments", tool_call.arguments_json}
                        }}
                    });
                }
                message_json["tool_calls"] = std::move(tool_calls_json);

                // 部分兼容接口要求 assistant 消息始终携带 content，这里显式补空串。
                message_json["content"] = message.content;
            } else {
                message_json["content"] = message.content;
            }

            messages_json.push_back(std::move(message_json));
        }

        return messages_json;
    }

    /**
     * @brief 将工具定义序列化为兼容模式 tools 字段
     * @param tools 工具列表
     * @return JSON 数组
     */
    static json build_tools_json(const std::vector<QwenToolDefinition>& tools) {
        json tools_json = json::array();

        for (const auto& tool : tools) {
            json function_json = {
                {"name", tool.name},
                {"description", tool.description}
            };

            if (!tool.parameters_json.empty()) {
                try {
                    function_json["parameters"] = json::parse(tool.parameters_json);
                } catch (...) {
                    function_json["parameters"] = json::object();
                }
            } else {
                function_json["parameters"] = json::object();
            }

            tools_json.push_back({
                {"type", "function"},
                {"function", std::move(function_json)}
            });
        }

        return tools_json;
    }

    /**
     * @brief 提取兼容模式响应中的 message.content
     * @param message_json 单条 message JSON
     * @return 文本内容
     */
    static std::string extract_message_content(const json& message_json) {
        if (!message_json.contains("content") || message_json["content"].is_null()) {
            return "";
        }

        if (message_json["content"].is_string()) {
            return message_json["content"].get<std::string>();
        }

        if (message_json["content"].is_array()) {
            std::string merged_content;
            for (const auto& item : message_json["content"]) {
                if (item.is_object() &&
                    item.value("type", "") == "text" &&
                    item.contains("text")) {
                    merged_content += item["text"].get<std::string>();
                } else if (item.is_string()) {
                    merged_content += item.get<std::string>();
                }
            }
            return merged_content;
        }

        return message_json["content"].dump();
    }

    /**
     * @brief 解析兼容模式返回的单轮结果
     * @param response 原始响应字符串
     * @return 解析后的响应对象
     */
    static QwenChatResult parse_chat_result(const std::string& response) {
        try {
            const json response_json = json::parse(response);

            if (response_json.contains("error")) {
                throw std::runtime_error(
                    "API Error: " + response_json["error"].value("message", "Unknown error"));
            }
            if (response_json.contains("code")) {
                throw std::runtime_error(
                    "API Error: " + response_json.value("message", "Unknown error"));
            }

            if (!response_json.contains("choices") ||
                !response_json["choices"].is_array() ||
                response_json["choices"].empty()) {
                throw std::runtime_error("Invalid response format");
            }

            const json& choice = response_json["choices"][0];
            if (!choice.contains("message")) {
                throw std::runtime_error("Invalid response format");
            }

            const json& message_json = choice["message"];
            QwenChatResult result;
            result.content = extract_message_content(message_json);

            if (message_json.contains("tool_calls") &&
                message_json["tool_calls"].is_array()) {
                for (const auto& tool_call_json : message_json["tool_calls"]) {
                    if (!tool_call_json.contains("function")) {
                        continue;
                    }

                    const json& function_json = tool_call_json["function"];
                    QwenToolCall tool_call;
                    tool_call.id = tool_call_json.value("id", "");
                    tool_call.name = function_json.value("name", "");

                    if (function_json.contains("arguments")) {
                        if (function_json["arguments"].is_string()) {
                            tool_call.arguments_json =
                                function_json["arguments"].get<std::string>();
                        } else {
                            tool_call.arguments_json = function_json["arguments"].dump();
                        }
                    }

                    result.tool_calls.push_back(std::move(tool_call));
                }
            }

            return result;
        } catch (const json::exception& e) {
            throw std::runtime_error(std::string("JSON parse error: ") + e.what());
        }
    }

    /**
     * @brief 发送 HTTP POST 请求
     * @param data 请求体
     * @return 原始响应字符串
     */
    std::string send_post_request(const std::string& data) {
        CURL* curl = curl_easy_init();
        if (!curl) {
            throw std::runtime_error("Failed to initialize CURL");
        }

        std::string response_data;

        // 设置请求头和鉴权信息，直接走百炼兼容模式接口。
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        const std::string auth_header = "Authorization: Bearer " + api_key_;
        headers = curl_slist_append(headers, auth_header.c_str());

        curl_easy_setopt(curl, CURLOPT_URL, api_url_.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

        const CURLcode res = curl_easy_perform(curl);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            throw std::runtime_error(std::string("CURL error: ") +
                                     curl_easy_strerror(res));
        }

        return response_data;
    }

    std::string api_key_;
    std::string model_;
    std::string api_url_;
};
