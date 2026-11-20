/**
 * @file src/platform/windows/input.cpp
 * @brief Definitions for input handling on Windows.
 */
#ifndef DOXYGEN
  #define WINVER 0x0A00
#endif
#ifdef DOXYGEN
  /**
   * @def CALLBACK
   * @brief Windows callback calling convention marker.
   */
  #define CALLBACK
#endif

// platform includes
#include <array>
#include <Windows.h>

// standard includes
#include <memory>
#include <vector>

// lib includes

// local includes
#include "misc.h"
#include "src/config.h"
#include "src/globals.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/platform/virtualhid_input.h"

namespace platf {
  using namespace std::literals;

  /**
   * @brief Global virtual input device handles shared by clients.
   */
  struct input_raw_t {
    virtualhid::input_context_t virtualhid;  ///< libvirtualhid input context.
  };

  input_t input() {
    return input_t {new input_raw_t {}};
  }

  virtualhid::input_context_t &virtualhid::get_input_context(input_t &input) {
    return input->virtualhid;
  }

  std::optional<util::point_t> get_mouse_loc(input_t & /*input*/) {
    POINT p;
    if (!GetCursorPos(&p)) {
      return std::nullopt;
    }

    return util::point_t {
      (double) p.x,
      (double) p.y
    };
  }

  /**
   * @brief Per-client virtual devices for touch and pen input.
   */
  struct client_input_raw_t: public client_input_t {
    /**
     * @brief Create per-client raw input devices for touch and pen events.
     *
     * @param input Platform input backend that receives the event.
     */
    explicit client_input_raw_t(input_t &input):
        virtualhid {input->virtualhid} {}

    virtualhid::client_context_t virtualhid;  ///< libvirtualhid client context.
  };

  /**
   * @brief Allocates a context to store per-client input data.
   * @param input The global input context.
   * @return A unique pointer to a per-client input data context.
   */
  std::unique_ptr<client_input_t> allocate_client_input_context(input_t &input) {
    return std::make_unique<client_input_raw_t>(input);
  }

  virtualhid::client_context_t &virtualhid::get_client_context(client_input_t *input) {
    return static_cast<client_input_raw_t *>(input)->virtualhid;
  }

  void freeInput(input_raw_t *input) {
    std::default_delete<input_raw_t> {}(input);
  }

  /**
   * @brief Returns the supported platform capabilities to advertise to the client.
   * @return Capability flags.
   */
  platform_caps::caps_t get_capabilities() {
    platform_caps::caps_t caps = 0;

    if (config::input.native_pen_touch) {
      const auto runtime = virtualhid::create_runtime();
      if (runtime) {
        const auto &capabilities = runtime->capabilities();
        if (capabilities.supports_touchscreen || capabilities.supports_pen_tablet) {
          caps |= platform_caps::pen_touch;
        }
      } else {
        BOOST_LOG(warning) << "Unable to create libvirtualhid runtime for touch/pen capability detection"sv;
      }
    }

    return caps;
  }
}  // namespace platf
