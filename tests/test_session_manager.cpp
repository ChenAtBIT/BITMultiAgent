#include "ai_orchestrator/session_manager.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <regex>

namespace {

using ai_orchestrator::dag::SessionManager;

class SessionManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() /
            ("dag-session-test-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    std::filesystem::path root_;
};

TEST_F(SessionManagerTest, CreatesRandomSessionAndAllScopedDirectories) {
    SessionManager sessions(root_);
    const auto first = sessions.create();
    const auto second = sessions.create();
    EXPECT_TRUE(std::regex_match(first, std::regex("^[a-f0-9]{32}$")));
    EXPECT_NE(first, second);
    EXPECT_TRUE(sessions.exists(first));
    EXPECT_TRUE(std::filesystem::is_directory(root_ / first / "workspace"));
    EXPECT_TRUE(std::filesystem::is_directory(root_ / first / "artifacts"));
    EXPECT_TRUE(std::filesystem::is_directory(root_ / first / "audit"));
}

TEST_F(SessionManagerTest, RestoresExistingSessionAfterServiceRestart) {
    std::string id;
    {
        SessionManager original(root_);
        id = original.create();
    }
    SessionManager restarted(root_);
    EXPECT_FALSE(restarted.exists(id));
    EXPECT_TRUE(restarted.resume(id));
    EXPECT_TRUE(restarted.exists(id));
}

TEST_F(SessionManagerTest, RejectsMalformedOrMissingSessionIds) {
    SessionManager sessions(root_);
    EXPECT_FALSE(sessions.resume("../escape"));
    EXPECT_FALSE(sessions.resume("0123456789abcdef0123456789abcdef"));
}

#ifndef _WIN32
TEST_F(SessionManagerTest, RejectsSessionDirectorySymlinkReplacement) {
    const std::string id = "0123456789abcdef0123456789abcdef";
    const auto outside = root_.parent_path() / (root_.filename().string() + "-outside");
    std::filesystem::create_directories(outside / "workspace");
    std::filesystem::create_directories(root_);
    std::filesystem::create_directory_symlink(outside, root_ / id);
    SessionManager sessions(root_);
    EXPECT_FALSE(sessions.resume(id));
    std::error_code error;
    std::filesystem::remove_all(outside, error);
}
#endif

}  // namespace
