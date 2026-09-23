#include "mocktail/graphics/chrome_trace_writer.h"

#include <gtest/gtest.h>
#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace mocktail {
namespace graphics {
namespace {

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    char pattern[] = "/tmp/mocktail_chrome_trace_XXXXXX";
    const char* created = mkdtemp(pattern);
    if (created != nullptr) root_ = created;
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  const std::filesystem::path& root() const { return root_; }

 private:
  std::filesystem::path root_;
};

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream content;
  content << input.rdbuf();
  return content.str();
}

nlohmann::json ParseTrace(const std::filesystem::path& path) {
  return nlohmann::json::parse(ReadFile(path));
}

std::unique_ptr<ChromeTraceWriter> OpenOrFail(
    const std::filesystem::path& path) {
  std::string error;
  auto writer = ChromeTraceWriter::Open(path, &error);
  EXPECT_NE(writer, nullptr) << error;
  return writer;
}

TEST(ChromeTraceWriterTest, WritesSliceAsCompleteEventInMicroseconds) {
  TemporaryDirectory directory;
  const auto path = directory.root() / "trace.json";
  auto writer = OpenOrFail(path);
  ASSERT_NE(writer, nullptr);

  writer->Slice("vkQueuePresentKHR", "present", 5'000'123'456ULL,
                5'002'623'456ULL, {{"frame", 7}, {"result", -4}});
  writer->Close();

  const nlohmann::json trace = ParseTrace(path);
  ASSERT_TRUE(trace.is_array());
  ASSERT_EQ(trace.size(), 1u);
  const nlohmann::json& event = trace[0];
  EXPECT_EQ(event["name"], "vkQueuePresentKHR");
  EXPECT_EQ(event["cat"], "present");
  EXPECT_EQ(event["ph"], "X");
  EXPECT_EQ(event["ts"], 5'000'123);
  EXPECT_EQ(event["dur"], 2'500);
  EXPECT_EQ(event["pid"], static_cast<int>(getpid()));
  EXPECT_EQ(event["tid"], static_cast<int>(gettid()));
  EXPECT_EQ(event["args"]["frame"], 7);
  EXPECT_EQ(event["args"]["result"], -4);
}

TEST(ChromeTraceWriterTest, WritesCounterSample) {
  TemporaryDirectory directory;
  const auto path = directory.root() / "trace.json";
  auto writer = OpenOrFail(path);
  ASSERT_NE(writer, nullptr);

  writer->Counter("frame interval (ms)", 9'000'000ULL, 16.5);
  writer->Close();

  const nlohmann::json trace = ParseTrace(path);
  ASSERT_EQ(trace.size(), 1u);
  EXPECT_EQ(trace[0]["name"], "frame interval (ms)");
  EXPECT_EQ(trace[0]["ph"], "C");
  EXPECT_EQ(trace[0]["ts"], 9'000);
  EXPECT_DOUBLE_EQ(trace[0]["args"]["value"].get<double>(), 16.5);
}

TEST(ChromeTraceWriterTest, EmptyTraceIsAnEmptyArray) {
  TemporaryDirectory directory;
  const auto path = directory.root() / "trace.json";
  auto writer = OpenOrFail(path);
  ASSERT_NE(writer, nullptr);

  writer->Close();

  EXPECT_EQ(ParseTrace(path), nlohmann::json::array());
}

TEST(ChromeTraceWriterTest, EscapesNames) {
  TemporaryDirectory directory;
  const auto path = directory.root() / "trace.json";
  auto writer = OpenOrFail(path);
  ASSERT_NE(writer, nullptr);

  writer->Slice("say \"hi\"\\now", "c\ta", 0, 1000);
  writer->Close();

  const nlohmann::json trace = ParseTrace(path);
  ASSERT_EQ(trace.size(), 1u);
  EXPECT_EQ(trace[0]["name"], "say \"hi\"\\now");
  EXPECT_EQ(trace[0]["cat"], "c\ta");
}

TEST(ChromeTraceWriterTest, FileIsValidJsonBetweenWrites) {
  TemporaryDirectory directory;
  const auto path = directory.root() / "trace.json";
  auto writer = OpenOrFail(path);
  ASSERT_NE(writer, nullptr);

  // A session killed at any of these points leaves a loadable trace.
  EXPECT_EQ(ParseTrace(path), nlohmann::json::array());
  writer->Slice("first", "test", 1000, 2000);
  writer->Flush();
  EXPECT_EQ(ParseTrace(path).size(), 1u);
  writer->Slice("second", "test", 3000, 4000);
  writer->Flush();

  const nlohmann::json trace = ParseTrace(path);
  ASSERT_EQ(trace.size(), 2u);
  EXPECT_EQ(trace[0]["name"], "first");
  EXPECT_EQ(trace[1]["name"], "second");
  writer->Close();
  EXPECT_EQ(ParseTrace(path), trace);
}

TEST(ChromeTraceWriterTest, DropsEventsAfterClose) {
  TemporaryDirectory directory;
  const auto path = directory.root() / "trace.json";
  auto writer = OpenOrFail(path);
  ASSERT_NE(writer, nullptr);

  writer->Slice("kept", "test", 0, 1);
  writer->Close();
  writer->Slice("dropped", "test", 2, 3);
  writer->Counter("dropped", 4, 1.0);
  writer->Flush();
  writer->Close();

  const nlohmann::json trace = ParseTrace(path);
  ASSERT_EQ(trace.size(), 1u);
  EXPECT_EQ(trace[0]["name"], "kept");
}

TEST(ChromeTraceWriterTest, KeepsEveryEventFromConcurrentThreads) {
  TemporaryDirectory directory;
  const auto path = directory.root() / "trace.json";
  auto writer = OpenOrFail(path);
  ASSERT_NE(writer, nullptr);

  constexpr int kThreads = 8;
  constexpr int kEventsPerThread = 20'000;
  std::vector<std::thread> threads;
  for (int thread = 0; thread < kThreads; ++thread) {
    threads.emplace_back([&writer, thread] {
      for (int event = 0; event < kEventsPerThread; ++event) {
        writer->Slice("work", "test", 1000, 2000,
                      {{"thread", thread}, {"event", event}});
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  writer->Close();

  const nlohmann::json trace = ParseTrace(path);
  ASSERT_EQ(trace.size(), static_cast<std::size_t>(kThreads) *
                              kEventsPerThread);
  std::set<std::pair<int, int>> seen;
  std::set<int> tids;
  for (const nlohmann::json& event : trace) {
    seen.emplace(event["args"]["thread"].get<int>(),
                 event["args"]["event"].get<int>());
    tids.insert(event["tid"].get<int>());
  }
  EXPECT_EQ(seen.size(), trace.size());
  EXPECT_EQ(tids.size(), static_cast<std::size_t>(kThreads));
}

TEST(ChromeTraceWriterTest, DestructorClosesTheArray) {
  TemporaryDirectory directory;
  const auto path = directory.root() / "trace.json";
  {
    auto writer = OpenOrFail(path);
    ASSERT_NE(writer, nullptr);
    writer->Slice("only", "test", 0, 1000);
  }
  EXPECT_EQ(ParseTrace(path).size(), 1u);
}

TEST(ChromeTraceWriterTest, OpenReportsUnwritablePath) {
  TemporaryDirectory directory;
  std::string error;

  const auto writer = ChromeTraceWriter::Open(
      directory.root() / "missing" / "trace.json", &error);

  EXPECT_EQ(writer, nullptr);
  EXPECT_NE(error.find("missing"), std::string::npos) << error;
}

TEST(ChromeTraceWriterTest, OpenCreatesOrTruncatesTheFile) {
  TemporaryDirectory directory;
  const auto path = directory.root() / "trace.json";
  std::ofstream(path) << "stale content that is not json";

  auto writer = OpenOrFail(path);
  ASSERT_NE(writer, nullptr);
  writer->Close();

  EXPECT_EQ(ParseTrace(path), nlohmann::json::array());
}

TEST(ChromeTraceWriterTest, ScopeRecordsItsLifetimeAndArgs) {
  TemporaryDirectory directory;
  const auto path = directory.root() / "trace.json";
  auto writer = OpenOrFail(path);
  ASSERT_NE(writer, nullptr);

  const std::uint64_t before = TraceClockNanos();
  {
    TraceScope scope(writer.get(), "scoped", "test");
    scope.Arg("count", 3);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  const std::uint64_t after = TraceClockNanos();
  writer->Close();

  const nlohmann::json trace = ParseTrace(path);
  ASSERT_EQ(trace.size(), 1u);
  const auto ts = trace[0]["ts"].get<std::uint64_t>();
  const auto dur = trace[0]["dur"].get<std::uint64_t>();
  EXPECT_GE(ts, before / 1000);
  EXPECT_GE(dur, 2'000u);
  EXPECT_LE(ts + dur, after / 1000 + 1);
  EXPECT_EQ(trace[0]["args"]["count"], 3);
}

TEST(ChromeTraceWriterTest, ScopeWithoutWriterRecordsNothing) {
  TraceScope scope(nullptr, "unused", "test");
  scope.Arg("ignored", 1);
}

std::size_t ThreadCount() {
  return static_cast<std::size_t>(
      std::distance(std::filesystem::directory_iterator("/proc/self/task"),
                    std::filesystem::directory_iterator()));
}

// A failed write closes the trace, so the writer thread has nothing left to
// do and exits instead of ticking for the rest of the process.
TEST(ChromeTraceWriterTest, WriterThreadExitsAfterAWriteFailure) {
  TemporaryDirectory directory;
  const auto path = directory.root() / "trace.json";
  const std::size_t idle_threads = ThreadCount();
  auto writer = OpenOrFail(path);
  ASSERT_NE(writer, nullptr);
  ASSERT_EQ(ThreadCount(), idle_threads + 1);

  // With SIGXFSZ ignored, a write past RLIMIT_FSIZE fails with EFBIG.
  struct sigaction ignore {};
  ignore.sa_handler = SIG_IGN;
  struct sigaction saved_action {};
  ASSERT_EQ(sigaction(SIGXFSZ, &ignore, &saved_action), 0);
  rlimit saved_limit{};
  ASSERT_EQ(getrlimit(RLIMIT_FSIZE, &saved_limit), 0);
  rlimit no_growth = saved_limit;
  no_growth.rlim_cur = 0;
  ASSERT_EQ(setrlimit(RLIMIT_FSIZE, &no_growth), 0);
  writer->Slice("lost", "test", 0, 1);
  writer->Flush();
  EXPECT_EQ(setrlimit(RLIMIT_FSIZE, &saved_limit), 0);
  EXPECT_EQ(sigaction(SIGXFSZ, &saved_action, nullptr), 0);

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (ThreadCount() != idle_threads &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_EQ(ThreadCount(), idle_threads);
  writer->Close();
}

TEST(ChromeTraceWriterTest, ActiveProfileTraceIsOffWithoutEnvironment) {
  ASSERT_EQ(std::getenv(kProfileTraceEnvironment), nullptr);
  EXPECT_EQ(ActiveProfileTrace(), nullptr);
}

TEST(ChromeTraceWriterTest, CpuClocksAdvanceWithWork) {
  // The process window encloses the thread window.
  const std::uint64_t process_before = TraceProcessCpuNanos();
  const std::uint64_t thread_before = TraceThreadCpuNanos();
  volatile std::uint64_t sink = 0;
  const auto until =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
  while (std::chrono::steady_clock::now() < until) {
    sink = sink + 1;
  }
  const std::uint64_t thread_used = TraceThreadCpuNanos() - thread_before;
  const std::uint64_t process_used = TraceProcessCpuNanos() - process_before;
  EXPECT_GE(thread_used, 10'000'000U);
  EXPECT_GE(process_used, thread_used);
}

TEST(FrameCpuSamplerTest, FirstPresentHasNoFrame) {
  FrameCpuSampler sampler;
  FrameCpuSample sample;
  EXPECT_FALSE(sampler.Sample(1, 10'000'000, 50'000'000, &sample));
}

TEST(FrameCpuSamplerTest, MeasuresCpuSpentSinceThePreviousPresent) {
  FrameCpuSampler sampler;
  FrameCpuSample sample;
  sampler.Sample(1, 10'000'000, 50'000'000, &sample);
  ASSERT_TRUE(sampler.Sample(1, 18'000'000, 90'000'000, &sample));
  EXPECT_TRUE(sample.has_thread);
  EXPECT_DOUBLE_EQ(sample.thread_ms, 8.0);
  EXPECT_DOUBLE_EQ(sample.process_ms, 40.0);
}

TEST(FrameCpuSamplerTest, SkipsThreadCpuWhenAnotherThreadPresentedLast) {
  FrameCpuSampler sampler;
  FrameCpuSample sample;
  sampler.Sample(1, 10'000'000, 50'000'000, &sample);
  ASSERT_TRUE(sampler.Sample(2, 3'000'000, 70'000'000, &sample));
  EXPECT_FALSE(sample.has_thread);
  EXPECT_DOUBLE_EQ(sample.process_ms, 20.0);

  // Thread 2 presented last, so its next frame measures from here.
  ASSERT_TRUE(sampler.Sample(2, 7'000'000, 80'000'000, &sample));
  EXPECT_TRUE(sample.has_thread);
  EXPECT_DOUBLE_EQ(sample.thread_ms, 4.0);
}

TEST(FrameCpuSamplerTest, ClocksReadOutOfOrderDoNotWrap) {
  // Two presenting threads may read the process clock in one order and take
  // the lock in the other; a reused thread id restarts its thread clock.
  FrameCpuSampler sampler;
  FrameCpuSample sample;
  sampler.Sample(1, 10'000'000, 50'000'000, &sample);
  ASSERT_TRUE(sampler.Sample(1, 4'000'000, 45'000'000, &sample));
  EXPECT_DOUBLE_EQ(sample.process_ms, 0.0);
  EXPECT_FALSE(sample.has_thread);

  // The later process reading stays the baseline.
  ASSERT_TRUE(sampler.Sample(1, 6'000'000, 60'000'000, &sample));
  EXPECT_DOUBLE_EQ(sample.process_ms, 10.0);
  EXPECT_TRUE(sample.has_thread);
  EXPECT_DOUBLE_EQ(sample.thread_ms, 2.0);
}

}  // namespace
}  // namespace graphics
}  // namespace mocktail
