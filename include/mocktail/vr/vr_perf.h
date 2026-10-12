#ifndef MOCKTAIL_VR_PERF_H_
#define MOCKTAIL_VR_PERF_H_

#include <cstdint>
#include <string>
#include <vector>

namespace mocktail::vr::perf {

// Opt-in, low-overhead stage timing and counter instrumentation. Collection is
// compiled out at runtime unless MOCKTAIL_VR_PERF is set, so the steady-state
// path only pays a single cached boolean test per record.
bool Enabled();

enum class Stage {
  kFrameCycle,
  kXrWaitFrame,
  kXrBeginFrame,
  kXrLocateViews,
  kSwapchainAcquire,
  kSwapchainWait,
  kCopyRecord,
  kCopySubmit,
  kCopyFenceWait,
  kMirrorPublish,
  kEvidence,
  kControllerSync,
  kStageCount,
};

enum class Counter {
  kPresents,
  kFramesBegun,
  kFramesSubmitted,
  kFramesSkipped,
  kSkipShouldRender,
  kSkipPoseInvalid,
  kSkipEyesUnbound,
  kSkipEyeFrameMismatch,
  kSubmits,
  kFenceWaits,
  kMirrorPairs,
  kMirrorDrops,
  kSkipVisibilityLost,
  kReferenceSpaceChanges,
  kVisibilityLost,
  kVisibilityGained,
  kFramesInvalidated,
  kControllerSyncs,
  kCounterCount,
};

const char* StageName(Stage stage);
const char* CounterName(Counter counter);

// Bounded reservoir per stage: the most recent N samples are kept and ranked
// only when a report is emitted (once per interval), never on the frame path.
class Collector {
 public:
  static constexpr std::size_t kReservoir = 4096;
  static constexpr std::uint64_t kIntervalNs = 1'000'000'000ULL;

  void Record(Stage stage, std::uint64_t nanos);
  void Count(Counter counter, std::uint64_t delta = 1);

  // Emits one distribution line per active stage plus a counter line when at
  // least kIntervalNs elapsed since the previous emission. Cheap no-op
  // otherwise. now_ns is a CLOCK_MONOTONIC timestamp supplied by the caller so
  // the collector stays clock-agnostic and testable.
  void EmitIfDue(std::uint64_t now_ns, const char* context);

  // Test/inspection accessors.
  std::uint64_t counter(Counter counter_id) const;
  std::vector<std::uint64_t> Percentiles(Stage stage) const;  // p50,p95,p99,max
  std::size_t Samples(Stage stage) const;
  void Reset();

 private:
  struct StageStat {
    std::vector<std::uint64_t> samples;  // ring, bounded by kReservoir
    std::size_t head = 0;
    std::size_t count = 0;
    std::uint64_t total_ns = 0;
  };
  StageStat stages_[static_cast<std::size_t>(Stage::kStageCount)]{};
  std::uint64_t counters_[static_cast<std::size_t>(Counter::kCounterCount)]{};
  std::uint64_t last_emit_ns_ = 0;
  std::uint64_t window_start_ns_ = 0;
};

// Process-wide collector used by the XR backend frame cycle.
Collector& ProcessCollector();

// Monotonic nanoseconds (CLOCK_MONOTONIC), 0 on failure.
std::uint64_t NowNs();

}  // namespace mocktail::vr::perf

#endif  // MOCKTAIL_VR_PERF_H_
