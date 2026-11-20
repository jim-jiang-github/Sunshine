/**
 * @file tests/unit/platform/test_virtualhid_input.cpp
 * @brief Tests for shared libvirtualhid input helpers.
 */

// test includes
#include "../../tests_common.h"
#include "../../tests_log_checker.h"

// standard includes
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

// lib includes
#include <lizardbyte/common/env.h>

// local includes
#include "src/config.h"
#include "src/globals.h"
#include "src/platform/virtualhid_input.h"

using namespace std::chrono_literals;
using namespace std::literals;

namespace {

  /**
   * @brief Fixture backed by libvirtualhid's observable in-memory devices.
   */
  class VirtualHidDeviceTest: public ::testing::Test {
  protected:
    /**
     * @brief Preserve input configuration and create fake virtual devices.
     */
    void SetUp() override {
      original_input = config::input;
      context_ = std::make_unique<platf::virtualhid::input_context_t>(lvh::BackendKind::fake);
      client_ = std::make_unique<platf::virtualhid::client_context_t>(*context_);

      ASSERT_NE(context_->runtime, nullptr);
      ASSERT_NE(context_->keyboard, nullptr);
      ASSERT_NE(context_->mouse, nullptr);
      ASSERT_NE(client_->touch, nullptr);
      ASSERT_NE(client_->pen, nullptr);
    }

    /**
     * @brief Restore process-wide input configuration.
     */
    void TearDown() override {
      client_.reset();
      context_.reset();
      config::input = std::move(original_input);
    }

    /**
     * @brief Access the fake global virtual HID context.
     *
     * @return Pointer to the fixture-owned global context.
     */
    platf::virtualhid::input_context_t *context() const {
      return context_.get();
    }

    /**
     * @brief Access the fake per-client virtual HID context.
     *
     * @return Pointer to the fixture-owned client context.
     */
    platf::virtualhid::client_context_t *client() const {
      return client_.get();
    }

  private:
    config::input_t original_input;  ///< Input configuration restored after each test.
    std::unique_ptr<platf::virtualhid::input_context_t> context_;  ///< Shared fake virtual devices.
    std::unique_ptr<platf::virtualhid::client_context_t> client_;  ///< Per-client fake touch and pen devices.
  };

}  // namespace

TEST(VirtualHidSeatTest, LeavesDeviceNameUnchangedForDefaultSeat) {
  EXPECT_EQ(platf::virtualhid::device_name_for_seat("libvirtualhid Keyboard"sv, ""sv), "libvirtualhid Keyboard");
  EXPECT_EQ(platf::virtualhid::device_name_for_seat("libvirtualhid Keyboard"sv, "seat0"sv), "libvirtualhid Keyboard");
}

TEST(VirtualHidSeatTest, AppendsNonDefaultSeatToDeviceName) {
  EXPECT_EQ(platf::virtualhid::device_name_for_seat("libvirtualhid Mouse"sv, "seat1"sv), "libvirtualhid Mouse (seat1)");
  EXPECT_EQ(platf::virtualhid::device_name_for_seat("Sunshine DualSense"sv, "seat_vr"sv), "Sunshine DualSense (seat_vr)");
}

namespace {

  /**
   * @brief Fixture that creates fake virtual devices while `XDG_SEAT` targets a secondary seat.
   */
  class VirtualHidSeatDeviceTest: public ::testing::Test {
  protected:
    /**
     * @brief Point `XDG_SEAT` at `seat1` before creating fake devices.
     */
    void SetUp() override {
      if (std::string value; lizardbyte::common::get_env("XDG_SEAT", value)) {
        original_seat_ = std::move(value);
      }
      original_input_ = config::input;
      ASSERT_EQ(lizardbyte::common::set_env("XDG_SEAT", "seat1"), 0);
      context_ = std::make_unique<platf::virtualhid::input_context_t>(lvh::BackendKind::fake);
      client_ = std::make_unique<platf::virtualhid::client_context_t>(*context_);
    }

    /**
     * @brief Release fake devices and restore the original seat and input configuration.
     */
    void TearDown() override {
      client_.reset();
      context_.reset();
      config::input = std::move(original_input_);
      if (original_seat_) {
        lizardbyte::common::set_env("XDG_SEAT", *original_seat_);
      } else {
        lizardbyte::common::unset_env("XDG_SEAT");
      }
    }

    std::unique_ptr<platf::virtualhid::input_context_t> context_;  ///< Shared fake virtual devices.
    std::unique_ptr<platf::virtualhid::client_context_t> client_;  ///< Per-client fake touch and pen devices.

  private:
    std::optional<std::string> original_seat_;  ///< `XDG_SEAT` value restored after each test.
    config::input_t original_input_;  ///< Input configuration restored after each test.
  };

}  // namespace

TEST_F(VirtualHidSeatDeviceTest, ScopesEveryDeviceNameToTargetSeat) {
  ASSERT_NE(context_->keyboard, nullptr);
  ASSERT_NE(context_->mouse, nullptr);
  ASSERT_NE(client_->touch, nullptr);
  ASSERT_NE(client_->pen, nullptr);
  EXPECT_EQ(context_->keyboard->profile().name, lvh::profiles::keyboard().name + " (seat1)");
  EXPECT_EQ(context_->mouse->profile().name, lvh::profiles::mouse().name + " (seat1)");
  EXPECT_EQ(client_->touch->profile().name, lvh::profiles::touchscreen().name + " (seat1)");
  EXPECT_EQ(client_->pen->profile().name, lvh::profiles::pen_tablet().name + " (seat1)");
}

TEST_F(VirtualHidDeviceTest, CreatesEveryDeviceWithFakeRuntime) {
  EXPECT_EQ(context()->runtime->backend_kind(), lvh::BackendKind::fake);
  EXPECT_EQ(context()->runtime->capabilities().backend_name, "fake");
  EXPECT_TRUE(context()->keyboard->is_open());
  EXPECT_TRUE(context()->mouse->is_open());
  EXPECT_TRUE(client()->touch->is_open());
  EXPECT_TRUE(client()->pen->is_open());

  const auto runtime = platf::virtualhid::create_runtime(lvh::BackendKind::fake);
  ASSERT_NE(runtime, nullptr);
  EXPECT_EQ(runtime->backend_kind(), lvh::BackendKind::fake);
}

TEST_F(VirtualHidDeviceTest, RoutesLibvirtualhidDiagnosticsToSunshineLog) {
  auto runtime = platf::virtualhid::create_runtime(lvh::BackendKind::fake);
  ASSERT_NE(runtime, nullptr);

  auto created = runtime->create_mouse();
  ASSERT_TRUE(created);
  ASSERT_NE(created.mouse, nullptr);
  EXPECT_TRUE(created.mouse->move_relative(1, -1).ok());
  EXPECT_FALSE(created.mouse->move_absolute(0, 0, 0, 1).ok());
  EXPECT_TRUE(created.mouse->close().ok());
  EXPECT_FALSE(created.mouse->move_relative(1, 1).ok());

  EXPECT_TRUE(log_checker::line_contains("test_sunshine.log", "Info: [libvirtualhid] initialized fake backend"));
  EXPECT_TRUE(log_checker::line_contains("test_sunshine.log", "Debug: [libvirtualhid] mouse "));
  EXPECT_TRUE(log_checker::line_contains("test_sunshine.log", "Warning: [libvirtualhid] rejected mouse event:"));
  EXPECT_TRUE(log_checker::line_contains("test_sunshine.log", "Error: [libvirtualhid] mouse input failed: mouse is closed"));
}

TEST_F(VirtualHidDeviceTest, TranslatesMouseAndKeyboardInput) {
  platf::virtualhid::move_mouse(*context(), -4, 7);
  auto mouse_event = context()->mouse->last_submitted_event();
  EXPECT_EQ(mouse_event.kind, lvh::MouseEventKind::relative_motion);
  EXPECT_EQ(mouse_event.x, -4);
  EXPECT_EQ(mouse_event.y, 7);

  const platf::touch_port_t viewport {
    .offset_x = 10,
    .offset_y = 20,
    .width = 3840,
    .height = 2160,
    .logical_width = 1920,
    .logical_height = 1080,
  };
  platf::virtualhid::abs_mouse(*context(), viewport, 10.6F, 20.4F);
  mouse_event = context()->mouse->last_submitted_event();
  EXPECT_EQ(mouse_event.kind, lvh::MouseEventKind::absolute_motion);
  EXPECT_EQ(mouse_event.x, 1);
  EXPECT_EQ(mouse_event.y, 0);
  EXPECT_NEAR(mouse_event.absolute_x, 0.6F, 0.0001F);
  EXPECT_NEAR(mouse_event.absolute_y, 0.4F, 0.0001F);
  EXPECT_EQ(mouse_event.width, 1920);
  EXPECT_EQ(mouse_event.height, 1080);

  constexpr std::array button_cases {
    std::pair {BUTTON_LEFT, lvh::MouseButton::left},
    std::pair {BUTTON_MIDDLE, lvh::MouseButton::middle},
    std::pair {BUTTON_RIGHT, lvh::MouseButton::right},
    std::pair {BUTTON_X1, lvh::MouseButton::side},
    std::pair {BUTTON_X2, lvh::MouseButton::extra},
  };
  for (const auto &[button, expected] : button_cases) {
    platf::virtualhid::button_mouse(*context(), button, false);
    mouse_event = context()->mouse->last_submitted_event();
    EXPECT_EQ(mouse_event.kind, lvh::MouseEventKind::button);
    EXPECT_EQ(mouse_event.button, expected);
    EXPECT_TRUE(mouse_event.pressed);
    platf::virtualhid::button_mouse(*context(), button, true);
    EXPECT_FALSE(context()->mouse->last_submitted_event().pressed);
  }
  const auto button_submit_count = context()->mouse->submit_count();
  platf::virtualhid::button_mouse(*context(), 999, false);
  EXPECT_EQ(context()->mouse->submit_count(), button_submit_count);

  platf::virtualhid::scroll(*context(), 120);
  mouse_event = context()->mouse->last_submitted_event();
  EXPECT_EQ(mouse_event.kind, lvh::MouseEventKind::vertical_scroll);
  EXPECT_EQ(mouse_event.high_resolution_scroll, 120);
  platf::virtualhid::hscroll(*context(), -240);
  mouse_event = context()->mouse->last_submitted_event();
  EXPECT_EQ(mouse_event.kind, lvh::MouseEventKind::horizontal_scroll);
  EXPECT_EQ(mouse_event.high_resolution_scroll, -240);

  config::input.always_send_scancodes = true;
  platf::virtualhid::keyboard_update(*context(), 0x41, false, 0);
  auto keyboard_event = context()->keyboard->last_submitted_event();
  EXPECT_EQ(keyboard_event.key_code, 0x41);
  EXPECT_TRUE(keyboard_event.pressed);
  EXPECT_FALSE(keyboard_event.extended);
#ifdef _WIN32
  EXPECT_TRUE(keyboard_event.uses_normalized_key_code);
  EXPECT_TRUE(keyboard_event.prefer_native_scan_code);
  platf::virtualhid::keyboard_update(*context(), 0x41, true, SS_KBE_FLAG_NON_NORMALIZED);
  keyboard_event = context()->keyboard->last_submitted_event();
  EXPECT_FALSE(keyboard_event.uses_normalized_key_code);
#else
  EXPECT_FALSE(keyboard_event.uses_normalized_key_code);
  EXPECT_FALSE(keyboard_event.prefer_native_scan_code);
  platf::virtualhid::keyboard_update(*context(), 0x41, true, 0xFF);
  keyboard_event = context()->keyboard->last_submitted_event();
#endif
  EXPECT_FALSE(keyboard_event.pressed);
  platf::virtualhid::keyboard_update(*context(), 0x0D, false, 0, true);
  keyboard_event = context()->keyboard->last_submitted_event();
  EXPECT_EQ(keyboard_event.key_code, 0x0D);
  EXPECT_TRUE(keyboard_event.extended);

  const auto keyboard_submit_count = context()->keyboard->submit_count();
  const std::string text = "Sunshine \u{2600}";
  platf::virtualhid::unicode(*context(), text.data(), static_cast<int>(text.size()));
  EXPECT_EQ(context()->keyboard->submit_count(), keyboard_submit_count + 1);
  platf::virtualhid::unicode(*context(), nullptr, 1);
  platf::virtualhid::unicode(*context(), text.data(), 0);
  EXPECT_EQ(context()->keyboard->submit_count(), keyboard_submit_count + 1);

  ASSERT_TRUE(context()->mouse->close().ok());
  ASSERT_TRUE(context()->keyboard->close().ok());
  platf::virtualhid::move_mouse(*context(), 1, 1);
  platf::virtualhid::keyboard_update(*context(), 0x41, false, 0);
  context()->mouse.reset();
  context()->keyboard.reset();
  platf::virtualhid::move_mouse(*context(), 1, 1);
  platf::virtualhid::move_mouse(*context(), viewport, 1, 1);
  platf::virtualhid::abs_mouse(*context(), viewport, 1.0F, 1.0F);
  platf::virtualhid::button_mouse(*context(), BUTTON_LEFT, false);
  platf::virtualhid::scroll(*context(), 1);
  platf::virtualhid::hscroll(*context(), 1);
  platf::virtualhid::keyboard_update(*context(), 0x41, false, 0);
  platf::virtualhid::unicode(*context(), text.data(), static_cast<int>(text.size()));
}

TEST_F(VirtualHidDeviceTest, RetargetsAbsoluteMouseWhenStreamedViewportChanges) {
  const platf::touch_port_t left_viewport {
    .offset_x = -1920,
    .offset_y = 0,
    .width = 3840,
    .height = 1080,
    .logical_width = 1920,
    .logical_height = 1080,
    .env_offset_x = -1920,
    .env_offset_y = 0,
  };

  const auto initial_mouse_id = context()->mouse->device_id();
  platf::virtualhid::abs_mouse(*context(), left_viewport, -960.25F, 540.5F);
  const auto left_mouse_id = context()->mouse->device_id();
  EXPECT_NE(left_mouse_id, initial_mouse_id);
  auto mouse_event = context()->mouse->last_submitted_event();
  EXPECT_FLOAT_EQ(mouse_event.absolute_x, 959.75F);
  EXPECT_FLOAT_EQ(mouse_event.absolute_y, 540.5F);
  EXPECT_EQ(mouse_event.width, 1920);
  EXPECT_EQ(mouse_event.height, 1080);

  platf::virtualhid::abs_mouse(*context(), left_viewport, -480.0F, 270.0F);
  EXPECT_EQ(context()->mouse->device_id(), left_mouse_id);

  auto right_viewport = left_viewport;
  right_viewport.offset_x = 0;
  platf::virtualhid::abs_mouse(*context(), right_viewport, 960.0F, 540.0F);
  EXPECT_NE(context()->mouse->device_id(), left_mouse_id);
  mouse_event = context()->mouse->last_submitted_event();
  EXPECT_FLOAT_EQ(mouse_event.absolute_x, 960.0F);
  EXPECT_FLOAT_EQ(mouse_event.absolute_y, 540.0F);
}

TEST_F(VirtualHidDeviceTest, RetargetsRelativeMouseWhenStreamedViewportChanges) {
  const platf::touch_port_t left_viewport {
    .offset_x = -1920,
    .offset_y = 0,
    .width = 3840,
    .height = 1080,
    .logical_width = 1920,
    .logical_height = 1080,
    .env_offset_x = -1920,
    .env_offset_y = 0,
  };

  const auto initial_mouse_id = context()->mouse->device_id();
  platf::virtualhid::move_mouse(*context(), left_viewport, 12, -7);
  const auto left_mouse_id = context()->mouse->device_id();
  EXPECT_NE(left_mouse_id, initial_mouse_id);
  auto mouse_event = context()->mouse->last_submitted_event();
  EXPECT_EQ(mouse_event.kind, lvh::MouseEventKind::relative_motion);
  EXPECT_EQ(mouse_event.x, 12);
  EXPECT_EQ(mouse_event.y, -7);

  platf::virtualhid::move_mouse(*context(), left_viewport, 3, 4);
  EXPECT_EQ(context()->mouse->device_id(), left_mouse_id);

  auto right_viewport = left_viewport;
  right_viewport.offset_x = 0;
  platf::virtualhid::move_mouse(*context(), right_viewport, -5, 9);
  EXPECT_NE(context()->mouse->device_id(), left_mouse_id);
  mouse_event = context()->mouse->last_submitted_event();
  EXPECT_EQ(mouse_event.kind, lvh::MouseEventKind::relative_motion);
  EXPECT_EQ(mouse_event.x, -5);
  EXPECT_EQ(mouse_event.y, 9);
}

TEST_F(VirtualHidDeviceTest, TranslatesTouchscreenLifecycleAndGeometry) {
  const platf::touch_port_t viewport {10, 20, 800, 600, 400, 300, 0, 0, "DP-1"};
  platf::touch_input_t touch {LI_TOUCH_EVENT_DOWN, 100, 7, -0.1F, 1.1F, 1.2F, 15.0F, 10.0F};
  platf::virtualhid::touch_update(*client(), viewport, touch);
  auto contact = client()->touch->last_submitted_contact();
  EXPECT_EQ(contact.id, 7);
  EXPECT_FLOAT_EQ(contact.x, 0.0F);
  EXPECT_FLOAT_EQ(contact.y, 1.0F);
  EXPECT_FLOAT_EQ(contact.pressure, 1.0F);
  EXPECT_EQ(contact.orientation, 80);
  EXPECT_TRUE(contact.touching);
  EXPECT_EQ(contact.viewport.offset_x, 10);
  EXPECT_EQ(contact.viewport.output_name, "DP-1");
  EXPECT_EQ(contact.viewport.offset_y, 20);
  EXPECT_EQ(contact.viewport.width, 800);
  EXPECT_EQ(contact.viewport.height, 600);
  EXPECT_FLOAT_EQ(contact.contact_major_axis, 15.0F);
  EXPECT_FLOAT_EQ(contact.contact_minor_axis, 10.0F);
  EXPECT_TRUE(client()->active_touches.contains(7));

  touch.eventType = LI_TOUCH_EVENT_HOVER;
  touch.rotation = 300;
  touch.pointerId = 8;
  touch.pressureOrDistance = 0.25F;
  platf::virtualhid::touch_update(*client(), viewport, touch);
  contact = client()->touch->last_submitted_contact();
  EXPECT_EQ(contact.orientation, -60);
  EXPECT_FALSE(contact.touching);
  EXPECT_TRUE(client()->active_touches.contains(8));

  touch.eventType = LI_TOUCH_EVENT_UP;
  platf::virtualhid::touch_update(*client(), viewport, touch);
  EXPECT_FALSE(client()->active_touches.contains(8));
  touch.eventType = LI_TOUCH_EVENT_CANCEL;
  touch.pointerId = 7;
  platf::virtualhid::touch_update(*client(), viewport, touch);
  EXPECT_FALSE(client()->active_touches.contains(7));

  touch.eventType = LI_TOUCH_EVENT_MOVE;
  touch.rotation = LI_ROT_UNKNOWN;
  touch.pointerId = 9;
  platf::virtualhid::touch_update(*client(), viewport, touch);
  EXPECT_EQ(client()->touch->last_submitted_contact().orientation, 0);
  touch.eventType = LI_TOUCH_EVENT_HOVER_LEAVE;
  platf::virtualhid::touch_update(*client(), viewport, touch);
  EXPECT_FALSE(client()->active_touches.contains(9));

  touch.eventType = LI_TOUCH_EVENT_DOWN;
  touch.pointerId = 10;
  platf::virtualhid::touch_update(*client(), viewport, touch);
  touch.pointerId = 11;
  platf::virtualhid::touch_update(*client(), viewport, touch);
  const auto before_cancel = client()->touch->submit_count();
  touch.eventType = LI_TOUCH_EVENT_CANCEL_ALL;
  platf::virtualhid::touch_update(*client(), viewport, touch);
  EXPECT_TRUE(client()->active_touches.empty());
  EXPECT_EQ(client()->touch->submit_count(), before_cancel + 2);

  const auto before_unknown = client()->touch->submit_count();
  touch.eventType = 0xFF;
  platf::virtualhid::touch_update(*client(), viewport, touch);
  EXPECT_EQ(client()->touch->submit_count(), before_unknown);
  ASSERT_TRUE(client()->touch->close().ok());
  touch.eventType = LI_TOUCH_EVENT_DOWN;
  platf::virtualhid::touch_update(*client(), viewport, touch);
  client()->touch.reset();
  platf::virtualhid::touch_update(*client(), viewport, touch);
}

TEST_F(VirtualHidDeviceTest, TranslatesPenButtonsToolsAndTransitions) {
  const platf::touch_port_t viewport {1, 2, 1000, 500, 1000, 500, 0, 0, "HDMI-A-1"};
  platf::pen_input_t pen {
    LI_TOUCH_EVENT_DOWN,
    LI_TOOL_TYPE_PEN,
    static_cast<std::uint8_t>(LI_PEN_BUTTON_PRIMARY | LI_PEN_BUTTON_SECONDARY),
    45,
    450,
    -0.25F,
    1.25F,
    1.5F,
    0.0F,
    0.0F,
  };
  platf::virtualhid::pen_update(*client(), viewport, pen);
  auto state = client()->pen->last_submitted_tool();
  EXPECT_EQ(state.tool, lvh::PenToolType::pen);
  EXPECT_FLOAT_EQ(state.x, 0.0F);
  EXPECT_FLOAT_EQ(state.y, 1.0F);
  EXPECT_FLOAT_EQ(state.pressure, 1.0F);
  EXPECT_FLOAT_EQ(state.distance, -1.0F);
  EXPECT_NEAR(state.tilt_x, -45.0F, 0.001F);
  EXPECT_NEAR(state.tilt_y, 0.0F, 0.001F);
  EXPECT_EQ(state.transition, lvh::PointerTransition::update);
  EXPECT_EQ(state.viewport.offset_x, 1);
  EXPECT_EQ(state.viewport.offset_y, 2);
  EXPECT_EQ(state.viewport.output_name, "HDMI-A-1");
  EXPECT_TRUE(client()->pressed_pen_buttons.contains(lvh::PenButton::primary));
  EXPECT_TRUE(client()->pressed_pen_buttons.contains(lvh::PenButton::secondary));

  const auto before_unchanged_buttons = client()->pen->submit_count();
  platf::virtualhid::pen_update(*client(), viewport, pen);
  EXPECT_EQ(client()->pen->submit_count(), before_unchanged_buttons + 1);

  pen.eventType = LI_TOUCH_EVENT_HOVER;
  pen.toolType = LI_TOOL_TYPE_ERASER;
  pen.penButtons = LI_PEN_BUTTON_TERTIARY;
  pen.tilt = LI_TILT_UNKNOWN;
  pen.rotation = LI_ROT_UNKNOWN;
  pen.pressureOrDistance = 0.4F;
  platf::virtualhid::pen_update(*client(), viewport, pen);
  state = client()->pen->last_submitted_tool();
  EXPECT_EQ(state.tool, lvh::PenToolType::eraser);
  EXPECT_FLOAT_EQ(state.pressure, -1.0F);
  EXPECT_FLOAT_EQ(state.distance, 0.4F);
  EXPECT_FLOAT_EQ(state.tilt_x, 0.0F);
  EXPECT_FLOAT_EQ(state.tilt_y, 0.0F);
  EXPECT_TRUE(client()->pressed_pen_buttons.contains(lvh::PenButton::tertiary));
  EXPECT_FALSE(client()->pressed_pen_buttons.contains(lvh::PenButton::primary));

  pen.eventType = LI_TOUCH_EVENT_UP;
  pen.toolType = LI_TOOL_TYPE_UNKNOWN;
  pen.penButtons = 0;
  platf::virtualhid::pen_update(*client(), viewport, pen);
  EXPECT_EQ(client()->pen->last_submitted_tool().tool, lvh::PenToolType::unchanged);
  EXPECT_EQ(client()->pen->last_submitted_tool().transition, lvh::PointerTransition::release);

  pen.eventType = LI_TOUCH_EVENT_HOVER_LEAVE;
  platf::virtualhid::pen_update(*client(), viewport, pen);
  EXPECT_EQ(client()->pen->last_submitted_tool().transition, lvh::PointerTransition::leave);
  pen.eventType = LI_TOUCH_EVENT_CANCEL;
  platf::virtualhid::pen_update(*client(), viewport, pen);
  EXPECT_EQ(client()->pen->last_submitted_tool().transition, lvh::PointerTransition::cancel);

  pen.eventType = LI_TOUCH_EVENT_DOWN;
  pen.penButtons = LI_PEN_BUTTON_PRIMARY;
  platf::virtualhid::pen_update(*client(), viewport, pen);
  ASSERT_FALSE(client()->pressed_pen_buttons.empty());
  pen.eventType = LI_TOUCH_EVENT_CANCEL_ALL;
  platf::virtualhid::pen_update(*client(), viewport, pen);
  EXPECT_TRUE(client()->pressed_pen_buttons.empty());
  EXPECT_EQ(client()->pen->last_submitted_tool().transition, lvh::PointerTransition::cancel);

  ASSERT_TRUE(client()->pen->close().ok());
  pen.eventType = LI_TOUCH_EVENT_DOWN;
  platf::virtualhid::pen_update(*client(), viewport, pen);
  client()->pen.reset();
  platf::virtualhid::pen_update(*client(), viewport, pen);
}

TEST_F(VirtualHidDeviceTest, PlatformWrappersForwardToVirtualHidContext) {
  auto platform_input = platf::input();
  ASSERT_TRUE(platform_input);
  auto &platform_context = platf::virtualhid::get_input_context(platform_input);
  platform_context = platf::virtualhid::input_context_t {lvh::BackendKind::fake};
  ASSERT_NE(platform_context.mouse, nullptr);
  ASSERT_NE(platform_context.keyboard, nullptr);

  const platf::touch_port_t viewport {0, 0, 1280, 720, 1280, 720};
  platf::move_mouse(platform_input, 2, 3);
  EXPECT_EQ(platform_context.mouse->last_submitted_event().kind, lvh::MouseEventKind::relative_motion);
  platf::abs_mouse(platform_input, viewport, 40.0F, 50.0F);
  EXPECT_EQ(platform_context.mouse->last_submitted_event().kind, lvh::MouseEventKind::absolute_motion);
  platf::button_mouse(platform_input, BUTTON_LEFT, false);
  EXPECT_EQ(platform_context.mouse->last_submitted_event().kind, lvh::MouseEventKind::button);
  platf::scroll(platform_input, 120);
  EXPECT_EQ(platform_context.mouse->last_submitted_event().kind, lvh::MouseEventKind::vertical_scroll);
  platf::hscroll(platform_input, -120);
  EXPECT_EQ(platform_context.mouse->last_submitted_event().kind, lvh::MouseEventKind::horizontal_scroll);
  platf::keyboard_update(platform_input, 0x41, false, 0);
  EXPECT_EQ(platform_context.keyboard->last_submitted_event().key_code, 0x41);
  platf::keyboard_update(platform_input, 0x0D, false, 0, true);
  EXPECT_TRUE(platform_context.keyboard->last_submitted_event().extended);
  const std::string text = "wrapper";
  const auto keyboard_count = platform_context.keyboard->submit_count();
  platf::unicode(platform_input, text.data(), static_cast<int>(text.size()));
  EXPECT_EQ(platform_context.keyboard->submit_count(), keyboard_count + 1);

  auto platform_client = platf::allocate_client_input_context(platform_input);
  ASSERT_NE(platform_client, nullptr);
  auto &platform_client_context = platf::virtualhid::get_client_context(platform_client.get());
  ASSERT_NE(platform_client_context.touch, nullptr);
  ASSERT_NE(platform_client_context.pen, nullptr);
  platf::touch_update(platform_client.get(), viewport, {LI_TOUCH_EVENT_DOWN, 0, 1, 0.25F, 0.5F, 1.0F, 2.0F, 1.0F});
  EXPECT_EQ(platform_client_context.touch->last_submitted_contact().id, 1);
  platf::pen_update(platform_client.get(), viewport, {LI_TOUCH_EVENT_HOVER, LI_TOOL_TYPE_PEN, 0, LI_TILT_UNKNOWN, LI_ROT_UNKNOWN, 0.25F, 0.5F, 0.5F, 0.0F, 0.0F});
  EXPECT_EQ(platform_client_context.pen->last_submitted_tool().tool, lvh::PenToolType::pen);
}
