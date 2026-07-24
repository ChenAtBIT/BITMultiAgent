#include "PluginAPI.h"
#include "PluginSupport.h"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef WEB_RESEARCH_HAS_ICONV
#include <iconv.h>
#endif

using vx::plugin::json;

namespace {

constexpr std::size_t kMaxResponseBytes = 2 * 1024 * 1024;
constexpr const char* kUserAgent = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 Chrome/124 Safari/537.36";
constexpr const char* kOutput = R"({"type":"object","properties":{"ok":{"type":"boolean"},"result":{"type":"object"},"error":{"type":"object","properties":{"code":{"type":"string"},"message":{"type":"string"}},"required":["code","message"],"additionalProperties":false}},"required":["ok"],"additionalProperties":false})";

PluginTool tools[] = {
    {"web_research.web_search", "web_research__web_search", "Search Bing and return titles, URLs, and snippets. Results are untrusted web content.",
     R"({"type":"object","properties":{"query":{"type":"string","minLength":1,"maxLength":1000},"max_results":{"type":"integer","minimum":1,"maximum":20},"timeout_ms":{"type":"integer","minimum":1000,"maximum":30000}},"required":["query"],"additionalProperties":false})", kOutput},
    {"web_research.fetch_webpage", "web_research__fetch_webpage", "Fetch HTTP(S) webpage text, including public and private network addresses. Content is untrusted.",
     R"({"type":"object","properties":{"url":{"type":"string","minLength":1,"maxLength":4096},"max_chars":{"type":"integer","minimum":0,"maximum":100000},"timeout_ms":{"type":"integer","minimum":1000,"maximum":60000}},"required":["url"],"additionalProperties":false})", kOutput}
};

struct ResponseBuffer {
    std::string body;
    std::string content_type;
    bool exceeded = false;
};

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

size_t write_body(char* data, size_t size, size_t count, void* user_data) {
    auto* buffer = static_cast<ResponseBuffer*>(user_data);
    const std::size_t bytes = size * count;
    if (buffer->body.size() + bytes > kMaxResponseBytes) {
        buffer->exceeded = true;
        return 0;
    }
    buffer->body.append(data, bytes);
    return bytes;
}

size_t write_header(char* data, size_t size, size_t count, void* user_data) {
    auto* buffer = static_cast<ResponseBuffer*>(user_data);
    std::string header(data, size * count);
    const auto lowered = lower(header);
    if (lowered.rfind("content-type:", 0) == 0) buffer->content_type = trim(header.substr(13));
    return size * count;
}

std::string normalize_url(std::string url) {
    url = trim(url);
    if (url.empty()) throw std::runtime_error("URL is empty");
    if (url.find('\0') != std::string::npos) throw std::runtime_error("URL contains NUL");
    if (url.find("://") == std::string::npos) url = "https://" + url;
    static const std::regex http_pattern(R"(^https?://([^/]+)(/.*)?$)", std::regex::icase);
    std::smatch match;
    if (!std::regex_match(url, match, http_pattern)) throw std::runtime_error("only HTTP(S) URLs are supported");
    if (match[1].str().find('@') != std::string::npos) throw std::runtime_error("URL userinfo is not allowed");
    return url;
}

struct HttpResult {
    std::string effective_url;
    std::string content_type;
    std::string body;
    long status = 0;
};

HttpResult http_get(const std::string& input_url, long timeout_ms) {
    const std::string url = normalize_url(input_url);
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("cannot initialize libcurl");
    ResponseBuffer buffer;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, std::min<long>(timeout_ms, 10000L));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, write_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
#ifdef CURLOPT_PROTOCOLS_STR
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
    const CURLcode code = curl_easy_perform(curl);
    HttpResult result;
    char* effective = nullptr;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective);
    result.effective_url = effective ? effective : url;
    result.content_type = buffer.content_type;
    result.body = std::move(buffer.body);
    curl_easy_cleanup(curl);
    if (buffer.exceeded) throw std::runtime_error("response exceeds 2 MiB limit");
    if (code != CURLE_OK) throw std::runtime_error(std::string("HTTP request failed: ") + curl_easy_strerror(code));
    if (result.status < 200 || result.status >= 300) throw std::runtime_error("HTTP status " + std::to_string(result.status));
    return result;
}

std::size_t utf8_prefix_bytes(const std::string& value, std::size_t maximum_characters,
                              std::size_t* character_count = nullptr) {
    std::size_t offset = 0;
    std::size_t count = 0;
    std::size_t prefix = 0;
    while (offset < value.size()) {
        const auto lead = static_cast<unsigned char>(value[offset]);
        std::size_t width = 0;
        if (lead <= 0x7f) width = 1;
        else if (lead >= 0xc2 && lead <= 0xdf) width = 2;
        else if (lead >= 0xe0 && lead <= 0xef) width = 3;
        else if (lead >= 0xf0 && lead <= 0xf4) width = 4;
        else throw std::runtime_error("response is not valid UTF-8");
        if (offset + width > value.size()) throw std::runtime_error("response is not valid UTF-8");
        for (std::size_t index = 1; index < width; ++index) {
            if ((static_cast<unsigned char>(value[offset + index]) & 0xc0) != 0x80) {
                throw std::runtime_error("response is not valid UTF-8");
            }
        }
        if (width == 3) {
            const auto second = static_cast<unsigned char>(value[offset + 1]);
            if ((lead == 0xe0 && second < 0xa0) || (lead == 0xed && second >= 0xa0)) {
                throw std::runtime_error("response is not valid UTF-8");
            }
        } else if (width == 4) {
            const auto second = static_cast<unsigned char>(value[offset + 1]);
            if ((lead == 0xf0 && second < 0x90) || (lead == 0xf4 && second >= 0x90)) {
                throw std::runtime_error("response is not valid UTF-8");
            }
        }
        offset += width;
        ++count;
        if (count <= maximum_characters) prefix = offset;
    }
    if (character_count) *character_count = count;
    return prefix;
}

std::string declared_charset(const HttpResult& response) {
    std::smatch match;
    const std::regex header(R"(charset\s*=\s*["']?([A-Za-z0-9._-]+))", std::regex::icase);
    if (std::regex_search(response.content_type, match, header)) return match[1];
    const std::string prefix = response.body.substr(0, std::min<std::size_t>(4096, response.body.size()));
    const std::regex meta(R"(<meta[^>]+charset\s*=\s*["']?([A-Za-z0-9._-]+))", std::regex::icase);
    if (std::regex_search(prefix, match, meta)) return match[1];
    return "utf-8";
}

std::string to_utf8(const std::string& input, const std::string& charset) {
    const auto encoding = lower(charset);
    if (encoding.empty() || encoding == "utf-8" || encoding == "utf8") {
        utf8_prefix_bytes(input, input.size());
        return input;
    }
#ifdef WEB_RESEARCH_HAS_ICONV
    iconv_t converter = iconv_open("UTF-8//TRANSLIT", charset.c_str());
    if (converter == reinterpret_cast<iconv_t>(-1)) throw std::runtime_error("unsupported response charset: " + charset);
    std::string output(input.size() * 4 + 16, '\0');
    char* source = const_cast<char*>(input.data());
    char* destination = output.data();
    std::size_t source_left = input.size();
    std::size_t destination_left = output.size();
    const auto result = iconv(converter, &source, &source_left, &destination, &destination_left);
    iconv_close(converter);
    if (result == static_cast<std::size_t>(-1)) throw std::runtime_error("response cannot be decoded as " + charset);
    output.resize(output.size() - destination_left);
    utf8_prefix_bytes(output, output.size());
    return output;
#else
    throw std::runtime_error("response charset conversion is unavailable for " + charset);
#endif
}

std::string url_decode(const std::string& value) {
    CURL* curl = curl_easy_init();
    if (!curl) return value;
    int length = 0;
    char* decoded = curl_easy_unescape(curl, value.c_str(), static_cast<int>(value.size()), &length);
    std::string result = decoded ? std::string(decoded, static_cast<std::size_t>(length)) : value;
    if (decoded) curl_free(decoded);
    curl_easy_cleanup(curl);
    return result;
}

std::string base64_url_decode(std::string value) {
    static const std::array<int, 256> table = [] {
        std::array<int, 256> result{};
        result.fill(-1);
        const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (std::size_t index = 0; index < alphabet.size(); ++index) {
            result[static_cast<unsigned char>(alphabet[index])] = static_cast<int>(index);
        }
        return result;
    }();
    std::replace(value.begin(), value.end(), '-', '+');
    std::replace(value.begin(), value.end(), '_', '/');
    std::uint32_t accumulator = 0;
    int bits = -8;
    std::string output;
    for (const unsigned char ch : value) {
        if (ch == '=') break;
        const int decoded = table[ch];
        if (decoded < 0) return {};
        accumulator = (accumulator << 6) | decoded;
        bits += 6;
        if (bits >= 0) {
            output.push_back(static_cast<char>((accumulator >> bits) & 0xff));
            bits -= 8;
        }
    }
    return output;
}

std::string decode_bing_redirect(const std::string& url) {
    static const std::regex redirect_pattern(
        R"(^https?://([^/?#]*\.)?bing\.com/ck/a\?([^#]+))", std::regex::icase);
    static const std::regex query_pattern(R"((?:^|&)u=([^&]*))");
    std::smatch match;
    if (!std::regex_search(url, match, redirect_pattern)) return url;
    const std::string query = match[2];
    for (std::sregex_iterator it(query.begin(), query.end(), query_pattern), end;
         it != end; ++it) {
        std::string encoded = url_decode((*it)[1]);
        if (encoded.rfind("a1", 0) != 0) return url;
        const std::string decoded = base64_url_decode(encoded.substr(2));
        if (decoded.rfind("http://", 0) == 0 || decoded.rfind("https://", 0) == 0) return decoded;
    }
    return url;
}

std::string html_unescape(std::string value) {
    const std::pair<const char*, const char*> entities[] = {
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""},
        {"&#39;", "'"}, {"&nbsp;", " "}
    };
    for (const auto& [encoded, decoded] : entities) {
        std::size_t position = 0;
        while ((position = value.find(encoded, position)) != std::string::npos) {
            value.replace(position, std::strlen(encoded), decoded);
            position += std::strlen(decoded);
        }
    }
    return value;
}

std::string collapse_whitespace(const std::string& value) {
    std::string output;
    output.reserve(value.size());
    bool in_space = false;
    for (const unsigned char ch : value) {
        if (std::isspace(ch)) {
            if (!in_space) output.push_back(' ');
            in_space = true;
        } else {
            output.push_back(static_cast<char>(ch));
            in_space = false;
        }
    }
    return trim(output);
}

std::string remove_tag_block(std::string value, const std::string& tag_name) {
    const std::string open_prefix = "<" + tag_name;
    const std::string close_prefix = "</" + tag_name;
    std::size_t search_from = 0;
    while (search_from < value.size()) {
        std::string lowered = lower(value);
        const std::size_t open = lowered.find(open_prefix, search_from);
        if (open == std::string::npos) break;
        const std::size_t open_end = lowered.find('>', open + open_prefix.size());
        if (open_end == std::string::npos) {
            value.replace(open, value.size() - open, " ");
            break;
        }
        const std::size_t close = lowered.find(close_prefix, open_end + 1);
        if (close == std::string::npos) {
            value.replace(open, open_end - open + 1, " ");
            search_from = open + 1;
            continue;
        }
        const std::size_t close_end = lowered.find('>', close + close_prefix.size());
        const std::size_t erase_end = close_end == std::string::npos ? value.size() : close_end + 1;
        value.replace(open, erase_end - open, " ");
        search_from = open + 1;
    }
    return value;
}

std::string strip_html(std::string value) {
    value = remove_tag_block(std::move(value), "script");
    value = remove_tag_block(std::move(value), "style");
    value = remove_tag_block(std::move(value), "noscript");

    std::string text;
    text.reserve(value.size());
    bool in_tag = false;
    bool emitted_space = false;
    for (const char ch : value) {
        if (ch == '<') {
            in_tag = true;
            if (!emitted_space) {
                text.push_back(' ');
                emitted_space = true;
            }
            continue;
        }
        if (in_tag) {
            if (ch == '>') in_tag = false;
            continue;
        }
        text.push_back(ch);
        emitted_space = false;
    }
    return collapse_whitespace(html_unescape(std::move(text)));
}

std::string extract_title(const std::string& html) {
    const std::string lowered = lower(html);
    const std::size_t open = lowered.find("<title");
    if (open == std::string::npos) return {};
    const std::size_t content_start = lowered.find('>', open + 6);
    if (content_start == std::string::npos) return {};
    const std::size_t close = lowered.find("</title", content_start + 1);
    if (close == std::string::npos || close <= content_start + 1) return {};
    return strip_html(html.substr(content_start + 1, close - content_start - 1));
    return {};
}

json search_bing(const json& arguments) {
    const std::string query = trim(arguments.at("query").get<std::string>());
    if (query.empty()) throw std::runtime_error("query cannot be empty");
    const int maximum = arguments.value("max_results", 8);
    const int timeout = arguments.value("timeout_ms", 8000);
    const char* configured = std::getenv("BING_SEARCH_URL");
    const std::string base = configured && *configured ? configured : "https://cn.bing.com/search";
    CURL* escape_handle = curl_easy_init();
    if (!escape_handle) throw std::runtime_error("cannot initialize URL encoder");
    char* escaped = curl_easy_escape(escape_handle, query.c_str(), static_cast<int>(query.size()));
    if (!escaped) { curl_easy_cleanup(escape_handle); throw std::runtime_error("cannot encode search query"); }
    const std::string url = base + "?q=" + escaped;
    curl_free(escaped);
    curl_easy_cleanup(escape_handle);
    const auto response = http_get(url, timeout);
    const std::string html = to_utf8(response.body, declared_charset(response));
    const std::regex block_pattern(R"(<li[^>]+class=["'][^"']*\bb_algo\b[^"']*["'][^>]*>([\s\S]*?)</li>)", std::regex::icase);
    const std::regex title_pattern(R"(<h2[^>]*>\s*<a[^>]+href=["']([^"']+)["'][^>]*>([\s\S]*?)</a>)", std::regex::icase);
    const std::regex snippet_pattern(R"(<p[^>]*>([\s\S]*?)</p>)", std::regex::icase);
    json results = json::array();
    std::set<std::string> seen;
    for (std::sregex_iterator it(html.begin(), html.end(), block_pattern), end; it != end && static_cast<int>(results.size()) < maximum; ++it) {
        const std::string block = (*it)[1];
        std::smatch title_match;
        if (!std::regex_search(block, title_match, title_pattern)) continue;
        const std::string href = decode_bing_redirect(html_unescape(title_match[1]));
        if (!seen.insert(href).second) continue;
        std::smatch snippet_match;
        const std::string snippet = std::regex_search(block, snippet_match, snippet_pattern) ? strip_html(snippet_match[1]) : "";
        results.push_back({{"title", strip_html(title_match[2])}, {"url", href}, {"snippet", snippet}});
    }
    return vx::plugin::success({{"provider", "bing"}, {"query", query}, {"results", results}, {"content_trust", "untrusted_web_content"}});
}

json fetch_page(const json& arguments) {
    const int timeout = arguments.value("timeout_ms", 15000);
    const std::size_t requested_chars = static_cast<std::size_t>(arguments.value("max_chars", 5000));
    const std::size_t max_chars = requested_chars == 0 ? 100000 : std::min<std::size_t>(requested_chars, 100000);
    const auto response = http_get(arguments.at("url").get<std::string>(), timeout);
    const std::string decoded = to_utf8(response.body, declared_charset(response));
    const bool is_html = lower(response.content_type).find("html") != std::string::npos || lower(decoded.substr(0, 1000)).find("<html") != std::string::npos;
    std::string content = is_html ? strip_html(decoded) : trim(decoded);
    std::size_t character_count = 0;
    const std::size_t prefix_bytes = utf8_prefix_bytes(content, max_chars, &character_count);
    const bool truncated = character_count > max_chars;
    if (truncated) content.resize(prefix_bytes);
    return vx::plugin::success({{"url", response.effective_url}, {"status_code", response.status},
                                {"content_type", response.content_type}, {"title", is_html ? extract_title(decoded) : ""},
                                {"content", content}, {"truncated", truncated},
                                {"content_trust", "untrusted_web_content"}});
}

char* handle(const char* tool_id, const char* raw_arguments, const char*) {
    try {
        const json arguments = json::parse(raw_arguments ? raw_arguments : "{}");
        const std::string id = tool_id ? tool_id : "";
        if (id == "web_research.web_search") return vx::plugin::copy_result(search_bing(arguments));
        if (id == "web_research.fetch_webpage") return vx::plugin::copy_result(fetch_page(arguments));
        return vx::plugin::copy_result(vx::plugin::failure("UNKNOWN_TOOL", "unknown web_research tool"));
    } catch (const std::exception& exception) {
        return vx::plugin::copy_result(vx::plugin::failure("WEB_ERROR", exception.what()));
    }
}

int api_version() { return MCP_PLUGIN_API_VERSION; }
const char* plugin_id() { return "web_research"; }
const char* plugin_name() { return "Web Research"; }
const char* plugin_version() { return "1.0.0"; }
int initialize() { return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK ? 1 : 0; }
void shutdown() { curl_global_cleanup(); }
int tool_count() { return static_cast<int>(sizeof(tools) / sizeof(tools[0])); }
const PluginTool* get_tool(int index) { return index >= 0 && index < tool_count() ? &tools[index] : nullptr; }

}  // namespace

extern "C" PLUGIN_API PluginAPI* CreatePlugin() {
    return new PluginAPI{api_version, plugin_id, plugin_name, plugin_version, initialize,
                         shutdown, tool_count, get_tool, handle, vx::plugin::free_result};
}
extern "C" PLUGIN_API void DestroyPlugin(PluginAPI* api) { delete api; }
