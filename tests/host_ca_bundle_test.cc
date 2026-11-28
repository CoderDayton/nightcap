#include "compat/host_ca_bundle.h"

#include <gtest/gtest.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

class HostCaBundleTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char root[] = "/tmp/mocktail_host_ca_XXXXXX";
    ASSERT_NE(::mkdtemp(root), nullptr);
    root_ = root;
  }
  void TearDown() override { std::filesystem::remove_all(root_); }

  std::string Write(const std::string& name, const std::string& contents) {
    const std::filesystem::path path = root_ / name;
    std::ofstream(path) << contents;
    return path.string();
  }

  std::filesystem::path root_;
};

TEST_F(HostCaBundleTest, ReturnsTheTargetOfASymlinkedBundle) {
  const std::string target = Write("tls-ca-bundle.pem", "-----BEGIN CERTIFICATE-----\n");
  const std::string link = (root_ / "ca-certificates.crt").string();
  ASSERT_EQ(::symlink(target.c_str(), link.c_str()), 0);

  const std::string candidates[] = {link};
  EXPECT_EQ(mocktail::compat::FindHostCaBundle(candidates),
            std::filesystem::canonical(target).string());
}

TEST_F(HostCaBundleTest, SkipsMissingEmptyAndDirectoryCandidates) {
  const std::string empty = Write("empty.pem", "");
  const std::string directory = (root_ / "dir").string();
  ASSERT_EQ(::mkdir(directory.c_str(), 0755), 0);
  const std::string good = Write("good.pem", "cert");

  const std::string candidates[] = {(root_ / "missing.pem").string(), empty,
                                    directory, "relative/good.pem", good};
  EXPECT_EQ(mocktail::compat::FindHostCaBundle(candidates),
            std::filesystem::canonical(good).string());
}

TEST_F(HostCaBundleTest, ReturnsEmptyWhenNoCandidateIsUsable) {
  const std::string candidates[] = {(root_ / "missing.pem").string()};
  EXPECT_EQ(mocktail::compat::FindHostCaBundle(candidates), "");
}

}  // namespace
