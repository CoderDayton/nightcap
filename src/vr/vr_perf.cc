#include "mocktail/vr/vr_perf.h"

#include <time.h>

#include <algorithm>
#include <cstdio>

namespace mocktail::vr::perf {
namespace {

Collector& Global() {
  static Collector collector;
  return collector;
}

}  // namespace

bool Enabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("MOCKTAIL_VR_PERF");
    return value != nullptr && value[0] != '\0' && *value != '0';
  }();
  if (!enabled) return false;
  static const std::uint64_t start = [] {
    const char* value = std::getenv("MOCKTAIL_VR_MEASURE_START_NS");
    return value ? std::strtoull(value, nullptr, 10) : 0ULL;
  }();
  static const std::uint64_t end = [] {
    const char* value = std::getenv("MOCKTAIL_VR_MEASURE_END_NS");
    return value ? std::strtoull(value, nullptr, 10) : ~0ULL;
  }();
  if (start == 0 && end == ~0ULL) return true;
  const auto now = NowNs();
  return now >= start && now <= end;
}

std::uint64_t NowNs() {
  timespec ts{};
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0;
  }
  return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
         static_cast<std::uint64_t>(ts.tv_nsec);
}

const char* StageName(Stage stage) {
  switch (stage) {
    case Stage::kFrameCycle: return "frame_cycle";
    case Stage::kXrWaitFrame: return "xr_wait_frame";
    case Stage::kXrBeginFrame: return "xr_begin_frame";
    case Stage::kXrLocateViews: return "xr_locate_views";
    case Stage::kSwapchainAcquire: return "swapchain_acquire";
    case Stage::kSwapchainWait: return "swapchain_wait";
    case Stage::kCopyRecord: return "copy_record";
    case Stage::kCopySubmit: return "copy_submit";
    case Stage::kCopyFenceWait: return "copy_fence_wait";
    case Stage::kMirrorPublish: return "mirror_publish";
    case Stage::kEvidence: return "evidence";
    case Stage::kControllerSync: return "controller_sync";
    case Stage::kStageCount: break;
  }
  return "unknown";
}

const char* CounterName(Counter counter) {
  switch (counter) {
    case Counter::kPresents: return "presents";
    case Counter::kFramesBegun: return "frames_begun";
    case Counter::kFramesSubmitted: return "frames_submitted";
    case Counter::kFramesSkipped: return "frames_skipped";
    case Counter::kSkipShouldRender: return "skip_should_render";
    case Counter::kSkipPoseInvalid: return "skip_pose_invalid";
    case Counter::kSkipEyesUnbound: return "skip_eyes_unbound";
    case Counter::kSkipEyeFrameMismatch: return "skip_eye_frame_mismatch";
    case Counter::kSubmits: return "submits";
    case Counter::kFenceWaits: return "fence_waits";
    case Counter::kMirrorPairs: return "mirror_pairs";
    case Counter::kMirrorDrops: return "mirror_drops";
    case Counter::kSkipVisibilityLost: return "skip_visibility_lost";
    case Counter::kReferenceSpaceChanges: return "reference_space_changes";
    case Counter::kVisibilityLost: return "visibility_lost";
    case Counter::kVisibilityGained: return "visibility_gained";
    case Counter::kFramesInvalidated: return "frames_invalidated";
    case Counter::kControllerSyncs: return "controller_syncs";
    case Counter::kCounterCount: break;
  }
  return "unknown";
}

void Collector::Record(Stage stage, std::uint64_t nanos) {
  if (!Enabled()) {
    return;
  }
  const std::size_t index = static_cast<std::size_t>(stage);
  if (index >= static_cast<std::size_t>(Stage::kStageCount)) {
    return;
  }
  StageStat& stat = stages_[index];
  if (stat.samples.size() < kReservoir) {
    stat.samples.resize(kReservoir, 0);
  }
  if (stat.count == kReservoir) {
    stat.total_ns -= stat.samples[stat.head];
  }
  stat.samples[stat.head] = nanos;
  stat.head = (stat.head + 1) % kReservoir;
  if (stat.count < kReservoir) {
    ++stat.count;
  }
  stat.total_ns += nanos;
}

void Collector::Count(Counter counter, std::uint64_t delta) {
  if (!Enabled()) {
    return;
  }
  const std::size_t index = static_cast<std::size_t>(counter);
  if (index >= static_cast<std::size_t>(Counter::kCounterCount)) {
    return;
  }
  counters_[index] += delta;
}

std::vector<std::uint64_t> Collector::Percentiles(Stage stage) const {
  const std::size_t index = static_cast<std::size_t>(stage);
  if (index >= static_cast<std::size_t>(Stage::kStageCount)) {
    return {};
  }
  const StageStat& stat = stages_[index];
  if (stat.count == 0) {
    return {0, 0, 0, 0};
  }
  std::vector<std::uint64_t> sorted(stat.samples.begin(),
                                    stat.samples.begin() + stat.count);
  std::sort(sorted.begin(), sorted.end());
  auto pick = [&sorted](double fraction) {
    const std::size_t position = static_cast<std::size_t>(
        fraction * static_cast<double>(sorted.size() - 1));
    return sorted[position];
  };
  return {pick(0.50), pick(0.95), pick(0.99), sorted.back()};
}

std::size_t Collector::Samples(Stage stage) const {
  const std::size_t index = static_cast<std::size_t>(stage);
  if (index >= static_cast<std::size_t>(Stage::kStageCount)) {
    return 0;
  }
  return stages_[index].count;
}

std::uint64_t Collector::counter(Counter counter_id) const {
  const std::size_t index = static_cast<std::size_t>(counter_id);
  if (index >= static_cast<std::size_t>(Counter::kCounterCount)) {
    return 0;
  }
  return counters_[index];
}

void Collector::Reset() {
  for (StageStat& stat : stages_) {
    stat.head = 0;
    stat.count = 0;
    stat.total_ns = 0;
  }
  for (std::uint64_t& value : counters_) {
    value = 0;
  }
  last_emit_ns_ = 0;
  window_start_ns_ = 0;
}

void Collector::EmitIfDue(std::uint64_t now_ns, const char* context) {
  if (!Enabled() || now_ns == 0) {
    return;
  }
  if (window_start_ns_ == 0) {
    window_start_ns_ = now_ns;
    last_emit_ns_ = now_ns;
    return;
  }
  if (now_ns - last_emit_ns_ < kIntervalNs) {
    return;
  }
  const double window_seconds =
      static_cast<double>(now_ns - window_start_ns_) / 1e9;
  std::fprintf(stderr, "  [vr-perf] window=%.1fs context=%s\n",
               window_seconds, context != nullptr ? context : "");
  for (std::size_t index = 0;
       index < static_cast<std::size_t>(Stage::kStageCount); ++index) {
    const StageStat& stat = stages_[index];
    if (stat.count == 0) {
      continue;
    }
    const std::vector<std::uint64_t> ranked =
        Percentiles(static_cast<Stage>(index));
    std::fprintf(stderr,
                 "  [vr-perf] stage=%-16s n=%zu avg=%.2fms p50=%.2fms "
                 "p95=%.2fms p99=%.2fms max=%.2fms\n",
                 StageName(static_cast<Stage>(index)), stat.count,
                 static_cast<double>(stat.total_ns) /
                     static_cast<double>(stat.count) / 1e6,
                 ranked[0] / 1e6, ranked[1] / 1e6, ranked[2] / 1e6,
                 ranked[3] / 1e6);
  }
  for (std::size_t index = 0;
       index < static_cast<std::size_t>(Counter::kCounterCount); ++index) {
    const std::uint64_t value = counters_[index];
    if (value == 0) {
      continue;
    }
    std::fprintf(stderr, "  [vr-perf] counter=%-22s total=%llu rate=%.2f/s\n",
                 CounterName(static_cast<Counter>(index)),
                 static_cast<unsigned long long>(value),
                 static_cast<double>(value) / std::max(window_seconds, 1e-9));
  }
  last_emit_ns_ = now_ns;
}

Collector& ProcessCollector() { return Global(); }

}  // namespace mocktail::vr::perf
