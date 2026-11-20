/**
 * @file tests/unit/test_input.cpp
 * @brief Tests for input packet validation, batching, and stream input behavior.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// lib includes
extern "C" {
#include <moonlight-common-c/src/Input.h>
}

// local includes
#include "src/config.h"
#include "src/input.h"
#include "src/platform/virtualhid_input.h"
#include "src/utility.h"

namespace {
  /**
   * @brief Protocol metadata for a fixed-size input packet.
   */
  struct input_packet_spec_t {
    std::uint32_t magic;  ///< Packet type identifier.
    std::size_t size;  ///< Complete fixed packet size.
  };

  /**
   * @brief Create raw input bytes with a caller-selected header and buffer size.
   *
   * @param magic Packet type identifier.
   * @param declared_size Packet size declared after the size field.
   * @param actual_size Number of bytes available in the packet buffer.
   * @return Raw packet bytes.
   */
  std::vector<std::uint8_t> make_input_packet(std::uint32_t magic, std::uint32_t declared_size, std::size_t actual_size) {
    std::vector<std::uint8_t> packet(actual_size);
    const NV_INPUT_HEADER header {
      util::endian::big(declared_size),
      util::endian::little(magic),
    };
    if (!packet.empty()) {
      std::memcpy(packet.data(), &header, std::min(packet.size(), sizeof(header)));
    }
    return packet;
  }

  /**
   * @brief Verify batching a repeated packet around a potentially different packet.
   *
   * @tparam Packet Fixed-size protocol packet type.
   * @param original Packet repeated before and after the different packet.
   * @param different Packet that may introduce a batching boundary.
   * @param magic Protocol identifier shared by the packets.
   * @param expected Expected number of dispatched packets.
   */
  template<class Packet>
  void expect_packet_dispatches(Packet original, Packet different, std::uint32_t magic, std::size_t expected) {
    SCOPED_TRACE(magic);
    auto stream_input = input::alloc(std::make_shared<safe::mail_raw_t>());
    ASSERT_NE(stream_input, nullptr);
    std::size_t dispatch_count = 0;
    input::testing::set_input_packet_hook([magic, &dispatch_count](std::uint32_t dispatched_magic) {
      EXPECT_EQ(dispatched_magic, magic);
      ++dispatch_count;
    });
    for (auto packet : {original, different, original}) {
      packet.header.size = util::endian::big<std::uint32_t>(sizeof(packet) - sizeof(packet.header.size));
      packet.header.magic = util::endian::little(magic);
      std::vector<std::uint8_t> bytes(sizeof(packet));
      std::memcpy(bytes.data(), &packet, sizeof(packet));
      input::passthrough(stream_input, std::move(bytes));
    }
    input::testing::process_queued_messages(stream_input);
    input::testing::set_input_packet_hook({});
    EXPECT_EQ(dispatch_count, expected);
    EXPECT_EQ(input::testing::queued_input_packet_count(stream_input), 0);
  }

  /**
   * @brief Fixture that installs a fake global virtual input backend.
   */
  class InputSessionTest: public ::testing::Test {
  protected:
    /**
     * @brief Preserve configuration and install observable fake devices.
     */
    void SetUp() override {
      original_input_ = config::input;
      config::input.mouse = true;

      auto platform_input = platf::input();
      ASSERT_TRUE(platform_input);
      auto &context = platf::virtualhid::get_input_context(platform_input);
      context = platf::virtualhid::input_context_t {lvh::BackendKind::fake};
      context_ = &context;
      runtime_ = context.runtime.get();
      ASSERT_NE(runtime_, nullptr);
      input::testing::set_platform_input(std::move(platform_input));
    }

    /**
     * @brief Restore configuration and release the fake platform input.
     */
    void TearDown() override {
      input::testing::set_input_packet_hook({});
      input::testing::set_input_task_sink({});
      input::testing::set_platform_input({});
      context_ = nullptr;
      runtime_ = nullptr;
      config::input = std::move(original_input_);
    }

    /**
     * @brief Access the fake runtime installed for the current test.
     *
     * @return Fake libvirtualhid runtime.
     */
    lvh::Runtime &runtime() const {
      return *runtime_;
    }

    /**
     * @brief Access the shared libvirtualhid input context installed for the test.
     *
     * @return Fake input context.
     */
    platf::virtualhid::input_context_t &context() const {
      return *context_;
    }

  private:
    platf::virtualhid::input_context_t *context_ = nullptr;  ///< Fake input context installed in the global backend.
    lvh::Runtime *runtime_ = nullptr;  ///< Fake runtime installed in the global input backend.
    config::input_t original_input_;  ///< Input configuration restored after each test.
  };
}  // namespace

TEST(InputPacketValidationTest, RejectsEveryBufferShorterThanTheHeader) {
  for (std::size_t actual_size = 0; actual_size < sizeof(NV_INPUT_HEADER); ++actual_size) {
    const auto packet = make_input_packet(UTF8_TEXT_EVENT_MAGIC, sizeof(std::uint32_t), actual_size);
    EXPECT_FALSE(input::testing::is_valid_input_packet(packet)) << "actual_size=" << actual_size;
  }
}

TEST(InputPacketValidationTest, RejectsDeclaredSizesOutsideTheAvailableBuffer) {
  constexpr std::uint32_t unknown_magic = 0x12345678;
  for (std::uint32_t declared_size = 0; declared_size < sizeof(std::uint32_t); ++declared_size) {
    const auto packet = make_input_packet(unknown_magic, declared_size, sizeof(NV_INPUT_HEADER));
    EXPECT_FALSE(input::testing::is_valid_input_packet(packet)) << "declared_size=" << declared_size;
  }

  const auto truncated_packet = make_input_packet(unknown_magic, sizeof(std::uint32_t) + 1, sizeof(NV_INPUT_HEADER));
  EXPECT_FALSE(input::testing::is_valid_input_packet(truncated_packet));

  const auto overflowing_size = make_input_packet(
    unknown_magic,
    std::numeric_limits<std::uint32_t>::max(),
    sizeof(NV_INPUT_HEADER)
  );
  EXPECT_FALSE(input::testing::is_valid_input_packet(overflowing_size));
}

TEST(InputPacketValidationTest, ValidatesUnicodePacketBoundsWithoutOverflow) {
  const auto empty_packet = make_input_packet(UTF8_TEXT_EVENT_MAGIC, sizeof(std::uint32_t), sizeof(NV_INPUT_HEADER));
  EXPECT_TRUE(input::testing::is_valid_input_packet(empty_packet));

  constexpr std::uint32_t maximum_declared_size = sizeof(std::uint32_t) + UTF8_TEXT_EVENT_MAX_COUNT;
  const auto maximum_packet = make_input_packet(
    UTF8_TEXT_EVENT_MAGIC,
    maximum_declared_size,
    sizeof(std::uint32_t) + maximum_declared_size
  );
  EXPECT_TRUE(input::testing::is_valid_input_packet(maximum_packet));

  const auto oversized_text = make_input_packet(
    UTF8_TEXT_EVENT_MAGIC,
    maximum_declared_size + 1,
    sizeof(std::uint32_t) + maximum_declared_size + 1
  );
  EXPECT_FALSE(input::testing::is_valid_input_packet(oversized_text));

  const auto truncated_text = make_input_packet(UTF8_TEXT_EVENT_MAGIC, maximum_declared_size, sizeof(NV_INPUT_HEADER));
  EXPECT_FALSE(input::testing::is_valid_input_packet(truncated_text));
}

TEST(InputPacketValidationTest, EnforcesEveryFixedPacketSize) {
  constexpr std::array packet_specs {
    input_packet_spec_t {MOUSE_MOVE_REL_MAGIC_GEN5, sizeof(NV_REL_MOUSE_MOVE_PACKET)},
    input_packet_spec_t {MOUSE_MOVE_ABS_MAGIC, sizeof(NV_ABS_MOUSE_MOVE_PACKET)},
    input_packet_spec_t {MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5, sizeof(NV_MOUSE_BUTTON_PACKET)},
    input_packet_spec_t {MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5, sizeof(NV_MOUSE_BUTTON_PACKET)},
    input_packet_spec_t {SCROLL_MAGIC_GEN5, sizeof(NV_SCROLL_PACKET)},
    input_packet_spec_t {SS_HSCROLL_MAGIC, sizeof(SS_HSCROLL_PACKET)},
    input_packet_spec_t {KEY_DOWN_EVENT_MAGIC, sizeof(NV_KEYBOARD_PACKET)},
    input_packet_spec_t {KEY_UP_EVENT_MAGIC, sizeof(NV_KEYBOARD_PACKET)},
    input_packet_spec_t {SS_TOUCH_MAGIC, sizeof(SS_TOUCH_PACKET)},
    input_packet_spec_t {SS_PEN_MAGIC, sizeof(SS_PEN_PACKET)},
  };

  for (const auto &[magic, size] : packet_specs) {
    const auto declared_size = static_cast<std::uint32_t>(size - sizeof(std::uint32_t));
    const auto valid_packet = make_input_packet(magic, declared_size, size);
    EXPECT_TRUE(input::testing::is_valid_input_packet(valid_packet)) << "magic=" << magic;

    const auto trailing_bytes = make_input_packet(magic, declared_size, size + 8);
    EXPECT_TRUE(input::testing::is_valid_input_packet(trailing_bytes)) << "magic=" << magic;

    const auto wrong_declared_size = make_input_packet(magic, declared_size - 1, size);
    EXPECT_FALSE(input::testing::is_valid_input_packet(wrong_declared_size)) << "magic=" << magic;

    const auto oversized_declared_size = make_input_packet(magic, declared_size + 1, size + 1);
    EXPECT_FALSE(input::testing::is_valid_input_packet(oversized_declared_size)) << "magic=" << magic;

    const auto truncated_packet = make_input_packet(magic, declared_size, size - 1);
    EXPECT_FALSE(input::testing::is_valid_input_packet(truncated_packet)) << "magic=" << magic;
  }
}

TEST(InputPacketValidationTest, PreservesUnknownPacketHandlingWithinDeclaredBounds) {
  constexpr std::uint32_t unknown_magic = 0x12345678;
  const auto packet = make_input_packet(unknown_magic, sizeof(std::uint32_t), sizeof(NV_INPUT_HEADER));
  EXPECT_TRUE(input::testing::is_valid_input_packet(packet));
}

TEST_F(InputSessionTest, RejectsMalformedBatchablePacketsAtQueueIngress) {
  ASSERT_FALSE(task_pool.running());
  const std::shared_ptr<input::input_t> empty_input;
  EXPECT_EQ(input::testing::queued_input_packet_count(empty_input), 0);

  auto stream_input = input::alloc(std::make_shared<safe::mail_raw_t>());
  ASSERT_NE(stream_input, nullptr);

  auto short_header = make_input_packet(SS_PEN_MAGIC, sizeof(std::uint32_t), sizeof(NV_INPUT_HEADER) - 1);
  input::passthrough(stream_input, std::move(short_header));
  EXPECT_EQ(input::testing::queued_input_packet_count(stream_input), 0);

  constexpr std::array batchable_packet_specs {
    input_packet_spec_t {MOUSE_MOVE_REL_MAGIC_GEN5, sizeof(NV_REL_MOUSE_MOVE_PACKET)},
    input_packet_spec_t {MOUSE_MOVE_ABS_MAGIC, sizeof(NV_ABS_MOUSE_MOVE_PACKET)},
    input_packet_spec_t {SCROLL_MAGIC_GEN5, sizeof(NV_SCROLL_PACKET)},
    input_packet_spec_t {SS_HSCROLL_MAGIC, sizeof(SS_HSCROLL_PACKET)},
    input_packet_spec_t {SS_TOUCH_MAGIC, sizeof(SS_TOUCH_PACKET)},
    input_packet_spec_t {SS_PEN_MAGIC, sizeof(SS_PEN_PACKET)},
  };

  std::size_t valid_packet_count = 0;
  for (const auto &[magic, size] : batchable_packet_specs) {
    const auto declared_size = static_cast<std::uint32_t>(size - sizeof(std::uint32_t));

    auto truncated_packet = make_input_packet(magic, declared_size, size - 1);
    input::passthrough(stream_input, std::move(truncated_packet));
    EXPECT_EQ(input::testing::queued_input_packet_count(stream_input), valid_packet_count) << "magic=" << magic;

    auto valid_packet = make_input_packet(magic, declared_size, size);
    input::passthrough(stream_input, std::move(valid_packet));
    ++valid_packet_count;
    EXPECT_EQ(input::testing::queued_input_packet_count(stream_input), valid_packet_count) << "magic=" << magic;

    auto oversized_declared_size = make_input_packet(magic, declared_size + 1, size + 1);
    input::passthrough(stream_input, std::move(oversized_declared_size));
    EXPECT_EQ(input::testing::queued_input_packet_count(stream_input), valid_packet_count) << "magic=" << magic;
  }
}

TEST_F(InputSessionTest, YieldsAfterBoundedPacketBatch) {
  ASSERT_FALSE(task_pool.running());
  auto stream_input = input::alloc(std::make_shared<safe::mail_raw_t>());
  ASSERT_NE(stream_input, nullptr);

  std::vector<std::shared_ptr<input::input_t>> scheduled;
  input::testing::set_input_task_sink([&](std::shared_ptr<input::input_t> input) {
    scheduled.push_back(std::move(input));
  });

  constexpr std::uint32_t unknown_magic = 0x12345678;
  constexpr std::size_t packet_count = 33;
  for (std::size_t n = 0; n < packet_count; ++n) {
    input::passthrough(stream_input, make_input_packet(unknown_magic, sizeof(std::uint32_t), sizeof(NV_INPUT_HEADER)));
  }
  ASSERT_EQ(scheduled.size(), 1);

  input::testing::process_queued_messages(scheduled.front());
  EXPECT_EQ(input::testing::queued_input_packet_count(stream_input), 1);
  ASSERT_EQ(scheduled.size(), 2);
  input::testing::process_queued_messages(scheduled.back());
  EXPECT_EQ(input::testing::queued_input_packet_count(stream_input), 0);
  EXPECT_EQ(scheduled.size(), 2);
}

TEST_F(InputSessionTest, BatchesContinuousPacketsWithoutCrossingStateChanges) {
  ASSERT_FALSE(task_pool.running());
  config::input.mouse = false;
  config::input.native_pen_touch = false;
  input::testing::set_input_task_sink([](std::shared_ptr<input::input_t>) {
  });

  SS_TOUCH_PACKET touch {};
  touch.eventType = LI_TOUCH_EVENT_MOVE;
  expect_packet_dispatches(touch, touch, SS_TOUCH_MAGIC, 1);
  auto changed_touch = touch;
  changed_touch.pointerId = 1;
  expect_packet_dispatches(touch, changed_touch, SS_TOUCH_MAGIC, 2);
  changed_touch = touch;
  changed_touch.eventType = LI_TOUCH_EVENT_DOWN;
  expect_packet_dispatches(touch, changed_touch, SS_TOUCH_MAGIC, 3);
  changed_touch.eventType = LI_TOUCH_EVENT_HOVER;
  expect_packet_dispatches(touch, changed_touch, SS_TOUCH_MAGIC, 3);

  SS_PEN_PACKET pen {};
  pen.eventType = LI_TOUCH_EVENT_MOVE;
  expect_packet_dispatches(pen, pen, SS_PEN_MAGIC, 1);
  auto changed_pen = pen;
  changed_pen.eventType = LI_TOUCH_EVENT_DOWN;
  expect_packet_dispatches(pen, changed_pen, SS_PEN_MAGIC, 3);
  changed_pen.eventType = LI_TOUCH_EVENT_HOVER;
  expect_packet_dispatches(pen, changed_pen, SS_PEN_MAGIC, 3);
  changed_pen = pen;
  changed_pen.penButtons = 1;
  expect_packet_dispatches(pen, changed_pen, SS_PEN_MAGIC, 3);
  changed_pen = pen;
  changed_pen.toolType = 1;
  expect_packet_dispatches(pen, changed_pen, SS_PEN_MAGIC, 3);
}

TEST_F(InputSessionTest, RefreshesSharedVirtualInputAfterLicenseStateChanges) {
  ASSERT_NE(context().keyboard, nullptr);
  ASSERT_NE(context().mouse, nullptr);
  const auto original_keyboard_id = context().keyboard->device_id();
  const auto original_mouse_id = context().mouse->device_id();
  const auto active_devices = runtime().active_device_count();

  input::refresh_virtual_input();

  ASSERT_NE(context().keyboard, nullptr);
  ASSERT_NE(context().mouse, nullptr);
  EXPECT_NE(context().keyboard->device_id(), original_keyboard_id);
  EXPECT_NE(context().mouse->device_id(), original_mouse_id);
  EXPECT_EQ(runtime().active_device_count(), active_devices);
}

TEST_F(InputSessionTest, RetargetsRelativeMouseFromLatestStreamGeometry) {
  auto session_mail = std::make_shared<safe::mail_raw_t>();
  auto stream_input = input::alloc(session_mail);
  ASSERT_NE(stream_input, nullptr);

  input::touch_port_t left_viewport {
    {
      .offset_x = -1920,
      .offset_y = 0,
      .width = 1920,
      .height = 1080,
      .logical_width = 1920,
      .logical_height = 1080,
      .env_offset_x = -1920,
      .env_offset_y = 0,
    },
    3840,
    1080,
    0.0F,
    0.0F,
    1.0F,
    1.0F,
    3840,
    1080,
  };

  const auto initial_mouse_id = context().mouse->device_id();
  session_mail->event<input::touch_port_t>(mail::touch_port)->raise(left_viewport);
  input::testing::send_relative_mouse_packet(stream_input, 12, -7);
  const auto left_mouse_id = context().mouse->device_id();
  EXPECT_NE(left_mouse_id, initial_mouse_id);
  auto mouse_event = context().mouse->last_submitted_event();
  EXPECT_EQ(mouse_event.kind, lvh::MouseEventKind::relative_motion);
  EXPECT_EQ(mouse_event.x, 12);
  EXPECT_EQ(mouse_event.y, -7);
  EXPECT_EQ(context().mouse_desktop.offset_x, -1920);
  EXPECT_EQ(context().mouse_desktop.width, 3840);
  EXPECT_EQ(context().mouse_viewport.offset_x, -1920);
  EXPECT_EQ(context().mouse_viewport.width, 1920);

  left_viewport.offset_x = 0;
  session_mail->event<input::touch_port_t>(mail::touch_port)->raise(left_viewport);
  input::testing::send_relative_mouse_packet(stream_input, -5, 9);
  EXPECT_NE(context().mouse->device_id(), left_mouse_id);
  mouse_event = context().mouse->last_submitted_event();
  EXPECT_EQ(mouse_event.kind, lvh::MouseEventKind::relative_motion);
  EXPECT_EQ(mouse_event.x, -5);
  EXPECT_EQ(mouse_event.y, 9);
  EXPECT_EQ(context().mouse_viewport.offset_x, 0);
}

TEST_F(InputSessionTest, TranslatesEditingKeysToCanonicalKeycodes) {
  config::input.keyboard = true;
  config::input.key_repeat_delay = std::chrono::milliseconds {0};

  auto mail = std::make_shared<safe::mail_raw_t>();
  auto session = input::alloc(mail);
  ASSERT_NE(session, nullptr);
  ASSERT_NE(context().keyboard, nullptr);

  constexpr std::uint8_t non_normalized = 0x01;  // SS_KBE_FLAG_NON_NORMALIZED

  struct editing_key_case {
    std::uint16_t client_key;  ///< Keycode sent by the client.
    std::uint16_t expected_key;  ///< Canonical keycode forwarded to the backend.
  };

  const std::array<editing_key_case, 8> cases {{
    {0x7A, 0x0100},  // SDL scancode Undo
    {0x7B, 0x0101},  // SDL scancode Cut
    {0x7C, 0x0102},  // SDL scancode Copy
    {0x7D, 0x0103},  // SDL scancode Paste
    {0x0100, 0x0100},  // Dedicated Undo
    {0x0101, 0x0101},  // Dedicated Cut
    {0x0102, 0x0102},  // Dedicated Copy
    {0x0103, 0x0103},  // Dedicated Paste
  }};

  for (const auto &editing_key : cases) {
    const auto before = context().keyboard->submit_count();

    input::testing::send_keyboard_packet(session, editing_key.client_key, 0, non_normalized, false);
    EXPECT_EQ(context().keyboard->submit_count(), before + 1);
    const auto press_event = context().keyboard->last_submitted_event();
    EXPECT_EQ(press_event.key_code, editing_key.expected_key);
    EXPECT_TRUE(press_event.pressed);

    input::testing::send_keyboard_packet(session, editing_key.client_key, 0, non_normalized, true);
    EXPECT_EQ(context().keyboard->submit_count(), before + 2);
    const auto release_event = context().keyboard->last_submitted_event();
    EXPECT_EQ(release_event.key_code, editing_key.expected_key);
    EXPECT_FALSE(release_event.pressed);
  }

  // Without the non-normalized flag, 0x7A-0x7D are the F11-F14 virtual keys and
  // must pass through untranslated.
  const auto before_fn = context().keyboard->submit_count();
  input::testing::send_keyboard_packet(session, 0x7A, 0, 0, false);
  EXPECT_EQ(context().keyboard->submit_count(), before_fn + 1);
  const auto fn_event = context().keyboard->last_submitted_event();
  EXPECT_EQ(fn_event.key_code, 0x7A);
  EXPECT_TRUE(fn_event.pressed);
  input::testing::send_keyboard_packet(session, 0x7A, 0, 0, true);

  // A normalized F11-F14 keycode (0x807A-0x807D) that also carries the
  // non-normalized flag must remain a function key and not be aliased to an
  // editing key. Only the low byte reaches the backend after prefix stripping.
  const std::array<std::uint16_t, 4> normalized_function_keys {0x807A, 0x807B, 0x807C, 0x807D};
  for (const auto normalized_key : normalized_function_keys) {
    const auto before = context().keyboard->submit_count();
    input::testing::send_keyboard_packet(session, normalized_key, 0, non_normalized, false);
    EXPECT_EQ(context().keyboard->submit_count(), before + 1);
    const auto event = context().keyboard->last_submitted_event();
    EXPECT_EQ(event.key_code, normalized_key & 0x00FF);
    EXPECT_TRUE(event.pressed);
    input::testing::send_keyboard_packet(session, normalized_key, 0, non_normalized, true);
  }
}

TEST_F(InputSessionTest, RetargetsNativeTouchAndPenWithoutApplyingTheMonitorOffsetTwice) {
  auto session_mail = std::make_shared<safe::mail_raw_t>();
  auto stream_input = input::alloc(session_mail);
  ASSERT_NE(stream_input, nullptr);
  auto &client = platf::virtualhid::get_client_context(input::testing::client_context(stream_input));
  ASSERT_NE(client.touch, nullptr);
  ASSERT_NE(client.pen, nullptr);
  input::testing::set_input_task_sink([](std::shared_ptr<input::input_t>) {
  });
  input::touch_port_t viewport {
    {.offset_x = -1920, .offset_y = -1080, .width = 2560, .height = 1080, .logical_width = 1920, .logical_height = 1080, .env_offset_x = -1920, .env_offset_y = -1080, .output_name = "DP-1"},
    7680,
    4320,
    320.0F,
    0.0F,
    2.0F,
    2.0F,
    3840,
    2160,
  };
  const auto write_float = [](netfloat &target, float value) {
    const auto bits = util::endian::little(std::bit_cast<std::uint32_t>(value));
    std::memcpy(target, &bits, sizeof(bits));
  };
  const auto send_packet = [&]<typename Packet>(Packet &packet, std::uint32_t magic) {
    packet.header.size = util::endian::big<std::uint32_t>(sizeof(packet) - sizeof(packet.header.size));
    packet.header.magic = util::endian::little(magic);
    std::vector<std::uint8_t> bytes(sizeof(packet));
    std::memcpy(bytes.data(), &packet, sizeof(packet));
    input::passthrough(stream_input, std::move(bytes));
    static_cast<void>(input::testing::process_queued_messages(stream_input));
  };
  SS_TOUCH_PACKET touch {};
  touch.eventType = LI_TOUCH_EVENT_DOWN;
  touch.pointerId = util::endian::little<std::uint32_t>(42);
  write_float(touch.x, 0.5F);
  write_float(touch.y, 0.25F);
  write_float(touch.pressureOrDistance, 0.75F);
  SS_PEN_PACKET pen {};
  pen.eventType = LI_TOUCH_EVENT_DOWN;
  pen.toolType = LI_TOOL_TYPE_PEN;
  write_float(pen.x, 0.5F);
  write_float(pen.y, 0.25F);
  write_float(pen.pressureOrDistance, 0.75F);
  for (const auto *name : {"DP-1", "HDMI-A-1"}) {
    viewport.output_name = name;
    session_mail->event<input::touch_port_t>(mail::touch_port)->raise(viewport);
    send_packet(touch, SS_TOUCH_MAGIC);
    const auto contact = client.touch->last_submitted_contact();
    EXPECT_EQ(contact.viewport.output_name, name);
    EXPECT_EQ(contact.viewport.offset_x, viewport.offset_x);
    EXPECT_EQ(contact.viewport.offset_y, viewport.offset_y);
    EXPECT_EQ(contact.viewport.width, 1920);
    EXPECT_EQ(contact.viewport.height, 1080);
    EXPECT_FLOAT_EQ(contact.x, 0.5F);
    EXPECT_FLOAT_EQ(contact.y, 0.25F);
    send_packet(pen, SS_PEN_MAGIC);
    const auto tool = client.pen->last_submitted_tool();
    EXPECT_EQ(tool.viewport.output_name, name);
    EXPECT_EQ(tool.viewport.offset_x, viewport.offset_x);
    EXPECT_EQ(tool.viewport.offset_y, viewport.offset_y);
    EXPECT_FLOAT_EQ(tool.x, 0.5F);
    EXPECT_FLOAT_EQ(tool.y, 0.25F);
    viewport.offset_x = 0;
    viewport.offset_y = 0;
    touch.eventType = LI_TOUCH_EVENT_MOVE;
    pen.eventType = LI_TOUCH_EVENT_MOVE;
  }
  // A capture backend can report monitor scaling before the desktop bounds are available.
  viewport.scalar_tpcoords = 1.0F;
  viewport.env_logical_width = 0;
  viewport.env_logical_height = 0;
  session_mail->event<input::touch_port_t>(mail::touch_port)->raise(viewport);
  send_packet(touch, SS_TOUCH_MAGIC);
  send_packet(pen, SS_PEN_MAGIC);
  EXPECT_FLOAT_EQ(client.touch->last_submitted_contact().x, 0.5F);
  EXPECT_FLOAT_EQ(client.touch->last_submitted_contact().y, 0.25F);
  EXPECT_FLOAT_EQ(client.pen->last_submitted_tool().x, 0.5F);
  EXPECT_FLOAT_EQ(client.pen->last_submitted_tool().y, 0.25F);
  viewport.scalar_tpcoords = 2.0F;
  viewport.logical_width = 0;
  viewport.logical_height = 0;
  session_mail->event<input::touch_port_t>(mail::touch_port)->raise(viewport);
  send_packet(touch, SS_TOUCH_MAGIC);
  send_packet(pen, SS_PEN_MAGIC);
  EXPECT_EQ(client.touch->last_submitted_contact().viewport.width, 1920);
  EXPECT_EQ(client.pen->last_submitted_tool().viewport.width, 1920);
  EXPECT_FLOAT_EQ(client.touch->last_submitted_contact().x, 0.5F);
  EXPECT_FLOAT_EQ(client.pen->last_submitted_tool().x, 0.5F);
  viewport.logical_width = 1920;
  session_mail->event<input::touch_port_t>(mail::touch_port)->raise(viewport);
  send_packet(touch, SS_TOUCH_MAGIC);
  EXPECT_EQ(client.touch->last_submitted_contact().viewport.width, 1920);

  const auto touch_count = client.touch->submit_count();
  const auto pen_count = client.pen->submit_count();
  viewport.logical_width = 0;
  viewport.client_offsetX = viewport.width / 2.0F;
  session_mail->event<input::touch_port_t>(mail::touch_port)->raise(viewport);
  send_packet(touch, SS_TOUCH_MAGIC);
  send_packet(pen, SS_PEN_MAGIC);
  EXPECT_EQ(client.touch->submit_count(), touch_count);
  EXPECT_EQ(client.pen->submit_count(), pen_count);
  viewport.client_offsetX = 0;
  viewport.client_offsetY = viewport.height / 2.0F;
  session_mail->event<input::touch_port_t>(mail::touch_port)->raise(viewport);
  send_packet(touch, SS_TOUCH_MAGIC);
  send_packet(pen, SS_PEN_MAGIC);
  EXPECT_EQ(client.touch->submit_count(), touch_count);
  EXPECT_EQ(client.pen->submit_count(), pen_count);
}
