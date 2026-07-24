#pragma once

#include <filesystem>
#include <mutex>
#include <set>
#include <string>

namespace ai_orchestrator::dag {

class SessionManager {
public:
    explicit SessionManager(std::filesystem::path root);

    std::string create();
    bool resume(const std::string& session_id);
    bool exists(const std::string& session_id) const;
    const std::filesystem::path& root() const { return root_; }

private:
    static bool valid_id(const std::string& value);
    bool ensure_directories(const std::string& session_id);

    std::filesystem::path root_;
    mutable std::mutex mutex_;
    std::set<std::string> active_;
};

}  // namespace ai_orchestrator::dag
