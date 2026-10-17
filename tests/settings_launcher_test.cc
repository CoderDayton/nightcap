#include "runtime/settings_launcher.h"

#include <gtest/gtest.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace mocktail {
namespace runtime {
namespace {

class MapEnvironment final : public Environment {
 public:
  explicit MapEnvironment(std::unordered_map<std::string, std::string> values)
      : values_(std::move(values)) {}

  std::optional<std::string> Get(std::string_view name) const override {
    const auto found = values_.find(std::string(name));
    return found == values_.end() ? std::nullopt
                                  : std::optional<std::string>(found->second);
  }

 private:
  std::unordered_map<std::string, std::string> values_;
};

class SettingsLauncherTest : public testing::Test {
 protected:
  void SetUp() override {
    char pattern[] = "/tmp/mocktail_settings_launcher_XXXXXX";
    const char* created = mkdtemp(pattern);
    ASSERT_NE(created, nullptr);
    directory_ = created;
  }

  void TearDown() override {
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
  }

  // A program that records that it ran, and with which arguments.
  std::filesystem::path WriteHelper(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << "#!/bin/sh\nprintf 'ran %s\\n' \"$*\" >'"
                        << (directory_ / "ran").string() << "'\n";
    chmod(path.c_str(), 0755);
    return path;
  }

  std::string Ran() {
    std::ostringstream contents;
    contents << std::ifstream(directory_ / "ran").rdbuf();
    return contents.str();
  }

  std::filesystem::path directory_;
};

TEST_F(SettingsLauncherTest, UsesTheProgramTheEnvironmentNames) {
  const std::filesystem::path helper = WriteHelper(directory_ / "settings");
  const MapEnvironment environment(
      {{"MOCKTAIL_SETTINGS_HELPER", helper.string()}});
  EXPECT_EQ(FindSettingsWindow(environment), helper);
}

TEST_F(SettingsLauncherTest, ANamedProgramThatCannotRunIsNotReplaced) {
  // A project root that would otherwise supply a program.
  WriteHelper(directory_ / "bin" / "mocktail_settings");
  for (const std::string named :
       {(directory_ / "missing").string(), std::string("relative/settings"),
        directory_.string()}) {
    const MapEnvironment environment(
        {{"MOCKTAIL_SETTINGS_HELPER", named},
         {"MOCKTAIL_PROJECT_ROOT", directory_.string()}});
    EXPECT_TRUE(FindSettingsWindow(environment).empty()) << named;
  }

  const std::filesystem::path target = WriteHelper(directory_ / "target");
  const std::filesystem::path link = directory_ / "link";
  std::filesystem::create_symlink(target, link);
  const MapEnvironment linked({{"MOCKTAIL_SETTINGS_HELPER", link.string()}});
  EXPECT_TRUE(FindSettingsWindow(linked).empty());
}

TEST_F(SettingsLauncherTest, FindsTheProgramUnderTheProjectRoot) {
  const std::filesystem::path helper =
      WriteHelper(directory_ / "bin" / "mocktail_settings");
  const MapEnvironment environment(
      {{"MOCKTAIL_PROJECT_ROOT", directory_.string()}});
  EXPECT_EQ(FindSettingsWindow(environment), helper);
}

TEST_F(SettingsLauncherTest, ExecReplacesTheProcessWithTheProgram) {
  const std::filesystem::path helper = WriteHelper(directory_ / "settings");
  const MapEnvironment environment(
      {{"MOCKTAIL_SETTINGS_HELPER", helper.string()}});
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    std::string error;
    ExecSettingsWindow(environment, &error);
    _exit(97);
  }
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
  EXPECT_EQ(Ran(), "ran \n");
}

TEST_F(SettingsLauncherTest, ExecReportsAProgramThatIsNotInstalled) {
  const MapEnvironment environment(
      {{"MOCKTAIL_SETTINGS_HELPER", (directory_ / "missing").string()}});
  std::string error;
  EXPECT_FALSE(ExecSettingsWindow(environment, &error));
  EXPECT_NE(error.find("settings"), std::string::npos) << error;
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
