// Modified by vii from komaruworld/mocktail. See README "About this fork".
#ifndef MOCKTAIL_PLATFORM_SDL_APPLICATION_METADATA_H_
#define MOCKTAIL_PLATFORM_SDL_APPLICATION_METADATA_H_

#include "mocktail/platform/application_id.h"
#include "mocktail/status.h"

namespace mocktail {
namespace platform {

inline constexpr char kMocktailApplicationName[] = "Nightcap";
inline constexpr const char* kMocktailApplicationIdentifier = kApplicationId;

// Configures the stable compositor identity before SDL initializes. The
// identifier matches the installed desktop file and icon name so Wayland and
// X11 compositors can group the runtime window with its launcher.
Status ConfigureSdlApplicationMetadata();

}  // namespace platform
}  // namespace mocktail

#endif  // MOCKTAIL_PLATFORM_SDL_APPLICATION_METADATA_H_
