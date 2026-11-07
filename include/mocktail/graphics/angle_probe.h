#ifndef MOCKTAIL_GRAPHICS_ANGLE_PROBE_H_
#define MOCKTAIL_GRAPHICS_ANGLE_PROBE_H_

#include <string>

#include "mocktail/graphics/graphics_backend.h"

namespace mocktail {
namespace graphics {

struct AngleProbeOptions {
  // Production callers provide paths from one pinned ANGLE distribution.
  // This probe intentionally does not scan browser installation directories.
  std::string egl_library_path;
  std::string gles_library_path;
};

// Checks ANGLE entry points and client extensions without initializing a display.
BackendCapability InspectAngleLibraries(const AngleProbeOptions& options);

}  // namespace graphics
}  // namespace mocktail

#endif  // MOCKTAIL_GRAPHICS_ANGLE_PROBE_H_
