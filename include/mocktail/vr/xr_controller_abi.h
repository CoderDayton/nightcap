#ifndef MOCKTAIL_VR_XR_CONTROLLER_ABI_H_
#define MOCKTAIL_VR_XR_CONTROLLER_ABI_H_

#include <stddef.h>
#include <stdint.h>

/* Plain-C mirror of mocktail::vr::ControllerDelivery for crossing the
 * dlsym module boundary (XR backend -> input runtime) without exporting C++
 * types or requiring the consumer to link OpenXR. Field order and types are
 * the ABI; both sides include this header. Keep it POD: no constructors, no
 * std, fixed arrays only. */

#ifdef __cplusplus
extern "C" {
#endif

#define MOCKTAIL_VR_CONTROLLER_MAX_EDGES 64
#define MOCKTAIL_VR_CONTROLLER_BUTTON_COUNT 8
#define MOCKTAIL_VR_CONTROLLER_AXIS_COUNT 6

/* Button indices: mirror mocktail::vr::ControllerButton enum order. */
enum {
  MOCKTAIL_VR_BUTTON_TRIGGER = 0,
  MOCKTAIL_VR_BUTTON_SQUEEZE = 1,
  MOCKTAIL_VR_BUTTON_THUMBSTICK = 2,
  MOCKTAIL_VR_BUTTON_FACE_NORTH = 3,
  MOCKTAIL_VR_BUTTON_FACE_SOUTH = 4,
  MOCKTAIL_VR_BUTTON_FACE_EAST = 5,
  MOCKTAIL_VR_BUTTON_FACE_WEST = 6,
  MOCKTAIL_VR_BUTTON_MENU = 7
};

typedef struct MocktailVrControllerEdge {
  int32_t hand;    /* 0 = left, 1 = right */
  int32_t button;  /* MOCKTAIL_VR_BUTTON_* */
  int32_t pressed; /* 0/1 */
} MocktailVrControllerEdge;

typedef struct MocktailVrControllerDelivery {
  uint64_t frame;
  uint64_t session_generation;
  int32_t connected[2];
  int32_t connection_changed[2];
  /* Current button levels per hand (MOCKTAIL_VR_BUTTON_* order). Used for
   * resync after queue overflow: the consumer re-delivers every level and the
   * router's deduplication makes it idempotent. */
  int32_t button_levels[2][MOCKTAIL_VR_CONTROLLER_BUTTON_COUNT];
  /* Analog levels in SDL gamepad axis order:
   * 0=LEFTX 1=LEFTY 2=RIGHTX 3=RIGHTY 4=TRIGGERLEFT 5=TRIGGERRIGHT. */
  float axes[MOCKTAIL_VR_CONTROLLER_AXIS_COUNT];
  int32_t axes_valid[2];
  int32_t resync;
  int32_t edge_count;
  MocktailVrControllerEdge edges[MOCKTAIL_VR_CONTROLLER_MAX_EDGES];
} MocktailVrControllerDelivery;

/* Pops the next delivery batch produced by the XR backend's once-per-frame
 * action sync. Returns 1 and fills *out when a batch was queued, 0 when the
 * queue is empty or no XR controller layer is active. Inert (returns 0) in
 * non-VR builds and while no backend is armed. */
typedef int32_t (*MocktailVrControllerPopFn)(MocktailVrControllerDelivery *out);

/* Queues a vibration request for one hand (0=left, 1=right). amplitude is
 * clamped to [0,1]; duration_ns 0 means the runtime default. Safe from any
 * thread; applied on the next XR sync. No-op when no controller layer is
 * active. */
typedef void (*MocktailVrControllerHapticsFn)(int32_t hand, float amplitude,
                                              uint64_t duration_ns,
                                              float frequency_hz);
typedef void (*MocktailVrControllerStopHapticsFn)(int32_t hand);

#ifdef __cplusplus
}
#endif

#endif /* MOCKTAIL_VR_XR_CONTROLLER_ABI_H_ */
