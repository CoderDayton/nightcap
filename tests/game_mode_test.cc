#include "runtime/game_mode.h"

#include <gtest/gtest.h>

#include <sched.h>

#include <cstddef>
#include <initializer_list>
#include <utility>
#include <vector>

namespace mocktail {
namespace runtime {
namespace {

cpu_set_t MaskOf(std::initializer_list<int> cpus) {
  cpu_set_t mask;
  CPU_ZERO(&mask);
  for (const int cpu : cpus) {
    CPU_SET(cpu, &mask);
  }
  return mask;
}

// Each get() consumes the next entry of `reads`, repeating the last one once
// the list runs out.
struct AffinityProbe {
  std::vector<cpu_set_t> reads;
  std::size_t get_calls = 0;
  int set_calls = 0;
  int get_result = 0;
  int set_result = 0;
  cpu_set_t last_set{};
};

AffinityProbe* g_affinity = nullptr;

int AffinityGet(cpu_set_t* mask) {
  const std::size_t index = g_affinity->get_calls++;
  if (g_affinity->get_result != 0 || g_affinity->reads.empty()) {
    return g_affinity->get_result != 0 ? g_affinity->get_result : -1;
  }
  const std::size_t last = g_affinity->reads.size() - 1;
  *mask = g_affinity->reads[index < last ? index : last];
  return 0;
}

int AffinitySet(const cpu_set_t* mask) {
  ++g_affinity->set_calls;
  g_affinity->last_set = *mask;
  return g_affinity->set_result;
}

CpuAffinityApi AffinityApi() { return CpuAffinityApi{&AffinityGet, &AffinitySet}; }

struct ClientProbe {
  int query_result = 0;
  int start_result = 0;
  int end_result = 0;
  int query_calls = 0;
  int start_calls = 0;
  int end_calls = 0;
  const char* error = "fixture failure";
};

ClientProbe* g_probe = nullptr;

int Query() {
  ++g_probe->query_calls;
  return g_probe->query_result;
}

int Start() {
  ++g_probe->start_calls;
  return g_probe->start_result;
}

int End() {
  ++g_probe->end_calls;
  return g_probe->end_result;
}

const char* Error() { return g_probe->error; }

GameModeClientApi Api() {
  return GameModeClientApi{&Start, &End, &Query, &Error};
}

TEST(GameModePolicyTest, ParsesCanonicalValuesAndBooleanAliases) {
  GameModePolicy policy = GameModePolicy::kOff;
  EXPECT_TRUE(ParseGameModePolicy("auto", &policy));
  EXPECT_EQ(policy, GameModePolicy::kAuto);
  EXPECT_TRUE(ParseGameModePolicy("on", &policy));
  EXPECT_EQ(policy, GameModePolicy::kOn);
  EXPECT_TRUE(ParseGameModePolicy("true", &policy));
  EXPECT_EQ(policy, GameModePolicy::kOn);
  EXPECT_TRUE(ParseGameModePolicy("off", &policy));
  EXPECT_EQ(policy, GameModePolicy::kOff);
  EXPECT_TRUE(ParseGameModePolicy("0", &policy));
  EXPECT_EQ(policy, GameModePolicy::kOff);
  EXPECT_FALSE(ParseGameModePolicy("required", &policy));
  EXPECT_FALSE(ParseGameModePolicy("auto", nullptr));
}

TEST(GameModeSessionTest, DisabledPolicyMakesNoClientCalls) {
  ClientProbe probe;
  g_probe = &probe;
  GameModeSession session = GameModeSession::StartWithClientForTesting(
      GameModePolicy::kOff, Api());

  EXPECT_EQ(session.state(), GameModeSessionState::kDisabled);
  EXPECT_TRUE(session.Stop().ok());
  EXPECT_EQ(probe.query_calls, 0);
  EXPECT_EQ(probe.start_calls, 0);
  EXPECT_EQ(probe.end_calls, 0);
}

TEST(GameModeSessionTest, OwnsExactlyOneSuccessfulRequest) {
  ClientProbe probe;
  g_probe = &probe;
  GameModeSession session = GameModeSession::StartWithClientForTesting(
      GameModePolicy::kAuto, Api());

  EXPECT_EQ(session.state(), GameModeSessionState::kActive);
  EXPECT_TRUE(session.owns_request());
  EXPECT_EQ(probe.query_calls, 1);
  EXPECT_EQ(probe.start_calls, 1);
  EXPECT_TRUE(session.Stop().ok());
  EXPECT_EQ(session.state(), GameModeSessionState::kStopped);
  EXPECT_EQ(probe.end_calls, 1);
  EXPECT_TRUE(session.Stop().ok());
  EXPECT_EQ(probe.end_calls, 1);
}

TEST(GameModeSessionTest, DestructorReleasesOwnedRequestOnEarlyReturn) {
  ClientProbe probe;
  g_probe = &probe;
  {
    GameModeSession session = GameModeSession::StartWithClientForTesting(
        GameModePolicy::kAuto, Api());
    ASSERT_TRUE(session.owns_request());
    EXPECT_EQ(probe.end_calls, 0);
  }
  EXPECT_EQ(probe.end_calls, 1);
}

TEST(GameModeSessionTest, AlreadyRegisteredProcessIsNotRequestedAgain) {
  ClientProbe probe;
  probe.query_result = 2;
  g_probe = &probe;
  GameModeSession session = GameModeSession::StartWithClientForTesting(
      GameModePolicy::kAuto, Api());

  EXPECT_EQ(session.state(), GameModeSessionState::kAlreadyActive);
  EXPECT_TRUE(session.active());
  EXPECT_FALSE(session.owns_request());
  EXPECT_EQ(probe.query_calls, 1);
  EXPECT_EQ(probe.start_calls, 0);
  EXPECT_TRUE(session.Stop().ok());
  EXPECT_EQ(probe.end_calls, 0);
}

TEST(GameModeSessionTest, FailedQueryStillAttemptsTheOptimization) {
  ClientProbe probe;
  probe.query_result = -1;
  g_probe = &probe;
  GameModeSession session = GameModeSession::StartWithClientForTesting(
      GameModePolicy::kOn, Api());

  EXPECT_EQ(session.state(), GameModeSessionState::kActive);
  EXPECT_EQ(probe.start_calls, 1);
  EXPECT_TRUE(session.Stop().ok());
  EXPECT_EQ(probe.end_calls, 1);
}

TEST(GameModeSessionTest, RejectedRequestIsFailOpenAndNeverEnded) {
  ClientProbe probe;
  probe.start_result = -1;
  probe.error = "daemon rejected fixture";
  g_probe = &probe;
  GameModeSession session = GameModeSession::StartWithClientForTesting(
      GameModePolicy::kOn, Api());

  EXPECT_EQ(session.state(), GameModeSessionState::kRequestFailed);
  EXPECT_EQ(session.detail(), "daemon rejected fixture");
  EXPECT_FALSE(session.active());
  EXPECT_TRUE(session.Stop().ok());
  EXPECT_EQ(probe.end_calls, 0);
}

TEST(GameModeSessionTest, MoveTransfersTheOwnedRequest) {
  ClientProbe probe;
  g_probe = &probe;
  GameModeSession first = GameModeSession::StartWithClientForTesting(
      GameModePolicy::kAuto, Api());
  GameModeSession second = std::move(first);

  EXPECT_FALSE(first.owns_request());
  EXPECT_TRUE(second.owns_request());
  EXPECT_TRUE(second.Stop().ok());
  EXPECT_EQ(probe.end_calls, 1);
}

TEST(GameModeSessionTest, StopFailureIsReportedOnlyOnce) {
  ClientProbe probe;
  probe.end_result = -1;
  probe.error = "daemon disappeared";
  g_probe = &probe;
  GameModeSession session = GameModeSession::StartWithClientForTesting(
      GameModePolicy::kAuto, Api());

  const Status stopped = session.Stop();
  EXPECT_FALSE(stopped.ok());
  EXPECT_EQ(stopped.message(), "daemon disappeared");
  EXPECT_EQ(session.state(), GameModeSessionState::kStopFailed);
  EXPECT_EQ(probe.end_calls, 1);
  EXPECT_TRUE(session.Stop().ok());
  EXPECT_EQ(probe.end_calls, 1);
}

TEST(CpuAffinityNarrowedTest, ComparesPermittedCpuSets) {
  const cpu_set_t all = MaskOf({0, 1, 2, 3});
  EXPECT_TRUE(CpuAffinityNarrowed(all, MaskOf({1, 2})));
  EXPECT_FALSE(CpuAffinityNarrowed(all, all));
  EXPECT_FALSE(CpuAffinityNarrowed(all, MaskOf({0, 1, 2, 3, 4})));
  // Same count, different CPUs: the daemon moved us without narrowing.
  EXPECT_FALSE(CpuAffinityNarrowed(MaskOf({0, 1}), MaskOf({2, 3})));
}

// The daemon re-applies its pin every few seconds, so a session that pins is
// dropped rather than corrected.
TEST(GameModeSessionTest, DropsTheRequestWhenTheDaemonPinsCores) {
  ClientProbe probe;
  g_probe = &probe;
  AffinityProbe affinity;
  affinity.reads = {MaskOf({0, 1, 2, 3}), MaskOf({2, 3})};
  g_affinity = &affinity;

  GameModeSession session = GameModeSession::StartWithClientForTesting(
      GameModePolicy::kAuto, Api(), AffinityApi());

  EXPECT_EQ(session.state(), GameModeSessionState::kDeclinedCorePinning);
  EXPECT_FALSE(session.active());
  EXPECT_FALSE(session.owns_request());
  EXPECT_EQ(probe.end_calls, 1);
  // request_end releases the pin; the explicit restore only covers a daemon
  // that leaves the mask behind.
  EXPECT_EQ(affinity.set_calls, 1);
  EXPECT_TRUE(CPU_EQUAL(&affinity.last_set, &affinity.reads[0]));
}

TEST(GameModeSessionTest, DroppedRequestIsNotEndedTwice) {
  ClientProbe probe;
  g_probe = &probe;
  AffinityProbe affinity;
  affinity.reads = {MaskOf({0, 1, 2, 3}), MaskOf({2, 3})};
  g_affinity = &affinity;

  GameModeSession session = GameModeSession::StartWithClientForTesting(
      GameModePolicy::kAuto, Api(), AffinityApi());

  ASSERT_EQ(probe.end_calls, 1);
  EXPECT_TRUE(session.Stop().ok());
  EXPECT_EQ(probe.end_calls, 1);
}

// A failed release leaves the daemon holding the request, so the session has
// to keep owning it and retry at shutdown instead of reporting it released.
TEST(GameModeSessionTest, KeepsAFailedReleaseForShutdown) {
  ClientProbe probe;
  probe.end_result = -1;
  probe.error = "release failed";
  g_probe = &probe;
  AffinityProbe affinity;
  affinity.reads = {MaskOf({0, 1, 2, 3}), MaskOf({2, 3})};
  g_affinity = &affinity;

  GameModeSession session = GameModeSession::StartWithClientForTesting(
      GameModePolicy::kAuto, Api(), AffinityApi());

  EXPECT_EQ(session.state(), GameModeSessionState::kDeclinedCorePinning);
  EXPECT_TRUE(session.owns_request());
  ASSERT_EQ(probe.end_calls, 1);
  EXPECT_FALSE(session.Stop().ok());
  EXPECT_EQ(probe.end_calls, 2);
}

TEST(GameModeSessionTest, KeepsTheRequestWhenAffinityIsUnchanged) {
  ClientProbe probe;
  g_probe = &probe;
  AffinityProbe affinity;
  affinity.reads = {MaskOf({0, 1, 2, 3})};
  g_affinity = &affinity;

  GameModeSession session = GameModeSession::StartWithClientForTesting(
      GameModePolicy::kAuto, Api(), AffinityApi());

  EXPECT_EQ(session.state(), GameModeSessionState::kActive);
  EXPECT_TRUE(session.owns_request());
  EXPECT_EQ(probe.end_calls, 0);
  EXPECT_EQ(affinity.set_calls, 0);
}

// `on` is an explicit request for GameMode, so its pinning is accepted.
TEST(GameModeSessionTest, ForcedPolicyKeepsAPinnedSession) {
  ClientProbe probe;
  g_probe = &probe;
  AffinityProbe affinity;
  affinity.reads = {MaskOf({0, 1, 2, 3}), MaskOf({2, 3})};
  g_affinity = &affinity;

  GameModeSession session = GameModeSession::StartWithClientForTesting(
      GameModePolicy::kOn, Api(), AffinityApi());

  EXPECT_EQ(session.state(), GameModeSessionState::kActive);
  EXPECT_TRUE(session.owns_request());
  EXPECT_EQ(probe.end_calls, 0);
  EXPECT_EQ(affinity.set_calls, 0);
}

// A user's own taskset is in place before the request, so a daemon pin inside
// it still counts as a narrowing and still drops the session.
TEST(GameModeSessionTest, NarrowingADeliberateMaskAlsoDropsTheRequest) {
  ClientProbe probe;
  g_probe = &probe;
  AffinityProbe affinity;
  affinity.reads = {MaskOf({0, 1}), MaskOf({1})};
  g_affinity = &affinity;

  GameModeSession session = GameModeSession::StartWithClientForTesting(
      GameModePolicy::kAuto, Api(), AffinityApi());

  EXPECT_EQ(session.state(), GameModeSessionState::kDeclinedCorePinning);
  EXPECT_TRUE(CPU_EQUAL(&affinity.last_set, &affinity.reads[0]));
}

TEST(GameModeSessionTest, RejectedRequestIsNotEvaluatedForPinning) {
  ClientProbe probe;
  probe.start_result = -1;
  g_probe = &probe;
  AffinityProbe affinity;
  affinity.reads = {MaskOf({0, 1, 2, 3}), MaskOf({2, 3})};
  g_affinity = &affinity;

  GameModeSession session = GameModeSession::StartWithClientForTesting(
      GameModePolicy::kOn, Api(), AffinityApi());

  EXPECT_EQ(session.state(), GameModeSessionState::kRequestFailed);
  EXPECT_EQ(probe.end_calls, 0);
  EXPECT_EQ(affinity.set_calls, 0);
}

// Another component owns the request, so dropping it is not ours to do.
TEST(GameModeSessionTest, AlreadyActiveProcessIsNotEvaluatedForPinning) {
  ClientProbe probe;
  probe.query_result = 2;
  g_probe = &probe;
  AffinityProbe affinity;
  affinity.reads = {MaskOf({0, 1, 2, 3}), MaskOf({2, 3})};
  g_affinity = &affinity;

  GameModeSession session = GameModeSession::StartWithClientForTesting(
      GameModePolicy::kAuto, Api(), AffinityApi());

  EXPECT_EQ(session.state(), GameModeSessionState::kAlreadyActive);
  EXPECT_EQ(probe.end_calls, 0);
  EXPECT_EQ(affinity.set_calls, 0);
}

TEST(GameModeSessionTest, UnreadableAffinityKeepsTheRequest) {
  ClientProbe probe;
  g_probe = &probe;
  AffinityProbe affinity;
  affinity.reads = {MaskOf({0, 1})};
  affinity.get_result = -1;
  g_affinity = &affinity;

  GameModeSession session = GameModeSession::StartWithClientForTesting(
      GameModePolicy::kAuto, Api(), AffinityApi());

  EXPECT_EQ(session.state(), GameModeSessionState::kActive);
  EXPECT_EQ(probe.end_calls, 0);
}

TEST(GameModeSessionTest, MissingAffinityApiKeepsTheRequest) {
  ClientProbe probe;
  g_probe = &probe;

  GameModeSession session = GameModeSession::StartWithClientForTesting(
      GameModePolicy::kAuto, Api(), CpuAffinityApi{});

  EXPECT_EQ(session.state(), GameModeSessionState::kActive);
  EXPECT_EQ(probe.end_calls, 0);
}

TEST(GameModeSessionTest, NamesTheDeclinedState) {
  EXPECT_STREQ(
      GameModeSessionStateName(GameModeSessionState::kDeclinedCorePinning),
      "declined-core-pinning");
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
