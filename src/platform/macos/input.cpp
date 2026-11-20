/**
 * @file src/platform/macos/input.cpp
 * @brief Definitions for libvirtualhid-backed macOS input handling.
 */

// platform includes
#include <ApplicationServices/ApplicationServices.h>

// standard includes
#include <cstdint>
#include <memory>

// lib includes
// local includes
#include "src/config.h"
#include "src/platform/virtualhid_input.h"

namespace platf {

  std::optional<util::point_t> get_mouse_loc(input_t & /*input*/) {
    const auto event = CGEventCreate(nullptr);
    if (!event) {
      return std::nullopt;
    }

    const auto current = CGEventGetLocation(event);
    CFRelease(event);
    return util::point_t {current.x, current.y};
  }

  platform_caps::caps_t get_capabilities() {
    platform_caps::caps_t caps = 0;
    const auto runtime = virtualhid::create_runtime();
    if (!runtime) {
      return caps;
    }

    const auto &capabilities = runtime->capabilities();
    if (config::input.native_pen_touch && (capabilities.supports_touchscreen || capabilities.supports_pen_tablet)) {
      caps |= platform_caps::pen_touch;
    }

    return caps;
  }

}  // namespace platf
