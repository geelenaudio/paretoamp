/**
 * Native unit tests – audio_util (gainDbToLinear), snapcast_protocol, eq_coeffs.
 * Run with: pio test -e native
 */

#include <unity.h>
#include "audio_util.h"
#include "snapcast_protocol.h"
#include "eq_coeffs.h"
#include "eq_types.h"
#include "idf_events.h"
#include "idf_mode_manager.h"
#include <cstring>
#include <cmath>
#include <type_traits>
#include <initializer_list>

void setUp(void) {}
void tearDown(void) {}

void test_gainDbToLinear_zero(void) {
  float v = geelen::gainDbToLinear(0);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.f, v);
}

void test_gainDbToLinear_negative_40(void) {
  float v = geelen::gainDbToLinear(-40);
  TEST_ASSERT_TRUE(v <= 0.f);
}

void test_gainDbToLinear_negative_12(void) {
  float v = geelen::gainDbToLinear(-12);
  float expected = powf(10.f, -12.f / 20.f);
  TEST_ASSERT_FLOAT_WITHIN(1e-5f, expected, v);
}

void test_gainDbToLinear_plus_6(void) {
  float v = geelen::gainDbToLinear(6);
  float expected = powf(10.f, 6.f / 20.f);
  TEST_ASSERT_FLOAT_WITHIN(1e-5f, expected, v);
}

void test_parseSnapcastBaseHeader_too_short(void) {
  uint8_t buf[10] = {0};
  uint16_t type = 0xFFFF;
  uint32_t payload_size = 0xFFFFFFFF;
  TEST_ASSERT_FALSE(geelen::parseSnapcastBaseHeader(buf, 10, type, payload_size));
}

void test_parseSnapcastBaseHeader_exact_26(void) {
  uint8_t buf[26];
  memset(buf, 0, sizeof(buf));
  buf[0] = 0x04;  /* type = 4 (WireChunk), little-endian */
  buf[1] = 0x00;
  buf[22] = 0x10;  /* payload_size = 16, little-endian */
  buf[23] = 0x00;
  buf[24] = 0x00;
  buf[25] = 0x00;
  uint16_t type = 0;
  uint32_t payload_size = 0;
  TEST_ASSERT_TRUE(geelen::parseSnapcastBaseHeader(buf, 26, type, payload_size));
  TEST_ASSERT_EQUAL_UINT16(4, type);
  TEST_ASSERT_EQUAL_UINT32(16, payload_size);
}

void test_parseSnapcastBaseHeader_large_payload(void) {
  uint8_t buf[26];
  memset(buf, 0, sizeof(buf));
  buf[0] = 0x00;
  buf[1] = 0x00;
  buf[22] = 0x00;
  buf[23] = 0x00;
  buf[24] = 0x02;  /* 0x00020000 = 131072 LE */
  buf[25] = 0x00;
  uint16_t type = 0;
  uint32_t payload_size = 0;
  TEST_ASSERT_TRUE(geelen::parseSnapcastBaseHeader(buf, 26, type, payload_size));
  TEST_ASSERT_EQUAL_UINT32(131072u, payload_size);
}

void test_buildSnapcastHelloFrame_roundtrip(void) {
  uint8_t out[512];
  size_t n = geelen::buildSnapcastHelloFrame(
      out, sizeof(out),
      "aa:bb:cc:dd:ee:ff",
      "geelen-amp",
      "Snapclient",
      "0.1.0",
      "ESP-IDF",
      "xtensa",
      1);
  TEST_ASSERT_GREATER_THAN_size_t(geelen::SNAPCAST_BASE_HEADER_SIZE + 4u, n);

  /* Parse the base header we just produced and confirm it decodes cleanly. */
  uint16_t type = 0;
  uint32_t payload_size = 0;
  TEST_ASSERT_TRUE(geelen::parseSnapcastBaseHeader(out, n, type, payload_size));
  TEST_ASSERT_EQUAL_UINT16(geelen::SNAPCAST_MSG_HELLO, type);

  /* payload_size must equal (total bytes) − header, and must match the
   * length of the JsonMessage uint32 prefix + JSON bytes. */
  TEST_ASSERT_EQUAL_UINT32((uint32_t)(n - geelen::SNAPCAST_BASE_HEADER_SIZE),
                           payload_size);

  /* The payload's first 4 bytes are the json length, LE. */
  const uint8_t* payload = out + geelen::SNAPCAST_BASE_HEADER_SIZE;
  const uint32_t json_len =
      (uint32_t)payload[0] | ((uint32_t)payload[1] << 8) |
      ((uint32_t)payload[2] << 16) | ((uint32_t)payload[3] << 24);
  TEST_ASSERT_EQUAL_UINT32(payload_size - 4u, json_len);

  /* Spot-check a few JSON fields by substring. This doesn't prove the JSON
   * is RFC-8259-valid, but with snprintf and ASCII-only inputs it is. */
  const char* json = (const char*)(payload + 4);
  TEST_ASSERT_NOT_NULL(strstr(json, "\"MAC\":\"aa:bb:cc:dd:ee:ff\""));
  TEST_ASSERT_NOT_NULL(strstr(json, "\"HostName\":\"geelen-amp\""));
  TEST_ASSERT_NOT_NULL(strstr(json, "\"ClientName\":\"Snapclient\""));
  TEST_ASSERT_NOT_NULL(strstr(json, "\"Arch\":\"xtensa\""));
  TEST_ASSERT_NOT_NULL(strstr(json, "\"OS\":\"ESP-IDF\""));
  TEST_ASSERT_NOT_NULL(strstr(json, "\"Version\":\"0.1.0\""));
  TEST_ASSERT_NOT_NULL(strstr(json, "\"SnapStreamProtocolVersion\":2"));
  TEST_ASSERT_NOT_NULL(strstr(json, "\"Instance\":1"));
}

void test_buildSnapcastHelloFrame_out_too_small(void) {
  uint8_t out[32];   /* way too small for even the base header+payload */
  size_t n = geelen::buildSnapcastHelloFrame(
      out, sizeof(out),
      "aa:bb:cc:dd:ee:ff", "host", "Snapclient", "0.1.0", "ESP-IDF",
      "xtensa", 1);
  TEST_ASSERT_EQUAL_size_t(0u, n);
}

void test_buildSnapcastHelloFrame_null_inputs(void) {
  uint8_t out[256];
  TEST_ASSERT_EQUAL_size_t(0u, geelen::buildSnapcastHelloFrame(
      out, sizeof(out), nullptr, "h", "c", "v", "o", "a", 1));
  TEST_ASSERT_EQUAL_size_t(0u, geelen::buildSnapcastHelloFrame(
      nullptr, sizeof(out), "m", "h", "c", "v", "o", "a", 1));
}

static geelen::EqBand makeBand(float f, float g, float q, geelen::EqFilterType ty, uint8_t slope) {
  geelen::EqBand b{};
  b.freq_hz = f;
  b.gain_db = g;
  b.q = q;
  b.type = ty;
  b.slope_db_oct = slope;
  return b;
}

void test_eqBuildStageList_bell_bypass_zero_gain(void) {
  geelen::EqBand bands[geelen::EQ_MAX_BANDS]{};
  bands[0] = makeBand(1000.f, 0.f, 1.2f, geelen::EqFilterType::Bell, 12);
  geelen::EqStage st[geelen::EQ_MAX_STAGES_PER_LAYER];
  size_t n = geelen::eqBuildStageList(44100.f, bands, 1, st);
  TEST_ASSERT_EQUAL_size_t(0u, n);
}

void test_eqBuildStageList_bell_three_db_one_stage(void) {
  geelen::EqBand bands[geelen::EQ_MAX_BANDS]{};
  bands[0] = makeBand(1000.f, 3.f, 1.2f, geelen::EqFilterType::Bell, 12);
  geelen::EqStage st[geelen::EQ_MAX_STAGES_PER_LAYER];
  size_t n = geelen::eqBuildStageList(44100.f, bands, 1, st);
  TEST_ASSERT_EQUAL_size_t(1u, n);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)geelen::EqStageKind::Biquad, (uint8_t)st[0].kind);
}

void test_eqBuildStageList_lowpass_24_two_biquads(void) {
  geelen::EqBand bands[geelen::EQ_MAX_BANDS]{};
  bands[0] = makeBand(5000.f, 0.f, 1.f, geelen::EqFilterType::LowPass, 24);
  geelen::EqStage st[geelen::EQ_MAX_STAGES_PER_LAYER];
  size_t n = geelen::eqBuildStageList(44100.f, bands, 1, st);
  TEST_ASSERT_EQUAL_size_t(2u, n);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)geelen::EqStageKind::Biquad, (uint8_t)st[0].kind);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)geelen::EqStageKind::Biquad, (uint8_t)st[1].kind);
}

void test_eqBuildStageList_lowpass_6_one_pole(void) {
  geelen::EqBand bands[geelen::EQ_MAX_BANDS]{};
  bands[0] = makeBand(1000.f, 0.f, 1.f, geelen::EqFilterType::LowPass, 6);
  geelen::EqStage st[geelen::EQ_MAX_STAGES_PER_LAYER];
  size_t n = geelen::eqBuildStageList(44100.f, bands, 1, st);
  TEST_ASSERT_EQUAL_size_t(1u, n);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)geelen::EqStageKind::OnePoleLp, (uint8_t)st[0].kind);
}

void test_eqLinearMagProduct_bell_boost_at_centre(void) {
  geelen::EqBand bands[geelen::EQ_MAX_BANDS]{};
  bands[0] = makeBand(1000.f, 6.f, 1.f, geelen::EqFilterType::Bell, 12);
  geelen::EqStage st[geelen::EQ_MAX_STAGES_PER_LAYER];
  size_t n = geelen::eqBuildStageList(44100.f, bands, 1, st);
  float lin = geelen::eqLinearMagProduct(st, n, 1000.f, 44100.f);
  float expected = powf(10.f, 6.f / 20.f);
  TEST_ASSERT_FLOAT_WITHIN(0.02f * expected, expected, lin);
}

/* --- Phase 0: event taxonomy stability ------------------------------------
 *
 * These checks exist so the native build catches accidental enum reorderings
 * or struct-size changes that would silently desynchronise producers and
 * handlers. The IDF-only event-loop machinery is not exercised here (no
 * esp_event on the host); on-device `supervisorBegin` is the smoke test for
 * actual dispatch end-to-end.
 */

void test_event_ids_are_stable(void) {
  /* Adding a new event at the end of the enum is fine. Reordering or
   * inserting in the middle changes the wire-style integer ids handlers
   * register against — guard with explicit equality so a careless edit
   * shows up in CI. */
  TEST_ASSERT_EQUAL_INT(0,  EV_NONE);
  TEST_ASSERT_EQUAL_INT(1,  EV_WIFI_UP);
  TEST_ASSERT_EQUAL_INT(2,  EV_WIFI_DOWN);
  TEST_ASSERT_EQUAL_INT(3,  EV_WIFI_AP_FALLBACK);
  TEST_ASSERT_EQUAL_INT(4,  EV_MODE_CHANGE_REQUESTED);
  TEST_ASSERT_EQUAL_INT(5,  EV_MODE_STARTED);
  TEST_ASSERT_EQUAL_INT(6,  EV_MODE_STOPPED);
  TEST_ASSERT_EQUAL_INT(7,  EV_VOICE_SCHEDULE_BOOT);
  TEST_ASSERT_EQUAL_INT(8,  EV_VOICE_PROMPT_FINISHED);
  TEST_ASSERT_EQUAL_INT(9,  EV_VOICE_BOOT_DONE);
  TEST_ASSERT_EQUAL_INT(10, EV_A2DP_CONNECTED);
  TEST_ASSERT_EQUAL_INT(11, EV_A2DP_DISCONNECTED);
  TEST_ASSERT_EQUAL_INT(12, EV_A2DP_PAIRING);
  TEST_ASSERT_EQUAL_INT(13, EV_SNAP_CONNECTED);
  TEST_ASSERT_EQUAL_INT(14, EV_SNAP_DISCONNECTED);
  TEST_ASSERT_EQUAL_INT(15, EV_DLNA_PLAY_REQUESTED);
  TEST_ASSERT_EQUAL_INT(16, EV_STATE_DIFF);
  TEST_ASSERT_EQUAL_INT(17, EV_COUNT_);
}

void test_event_payloads_are_pod(void) {
  /* `esp_event_post_to` does an internal `memcpy(len)` of the payload into
   * the loop's queue ring, so payload structs MUST be standard layout +
   * trivially copyable. C++17 type traits cover both properties. */
  TEST_ASSERT_TRUE(std::is_standard_layout<ev_wifi_up_t>::value);
  TEST_ASSERT_TRUE(std::is_trivially_copyable<ev_wifi_up_t>::value);
  TEST_ASSERT_TRUE(std::is_standard_layout<ev_wifi_down_t>::value);
  TEST_ASSERT_TRUE(std::is_trivially_copyable<ev_wifi_down_t>::value);
  TEST_ASSERT_TRUE(std::is_standard_layout<ev_mode_change_t>::value);
  TEST_ASSERT_TRUE(std::is_trivially_copyable<ev_mode_change_t>::value);
  TEST_ASSERT_TRUE(std::is_standard_layout<ev_voice_prompt_t>::value);
  TEST_ASSERT_TRUE(std::is_trivially_copyable<ev_voice_prompt_t>::value);
  TEST_ASSERT_TRUE(std::is_standard_layout<ev_dlna_play_t>::value);
  TEST_ASSERT_TRUE(std::is_trivially_copyable<ev_dlna_play_t>::value);
  TEST_ASSERT_TRUE(std::is_standard_layout<ev_state_diff_t>::value);
  TEST_ASSERT_TRUE(std::is_trivially_copyable<ev_state_diff_t>::value);
}

/* --- Phase 5: Mode FSM transition policy ----------------------------------
 *
 * Pins the Mode Manager's pure decision function so a careless edit to the
 * "reboot vs hot-swap" matrix shows up in CI before it reaches a flashed
 * device. Today every cross-mode transition rebooots; once `IProducer`
 * lands for snap / test-tone, the diagonal block of non-Bluetooth pairs
 * flips to HotSwap and these tests update accordingly.
 */

void test_mode_fsm_noop_for_same_source(void) {
  using geelen_idf::ModeTransitionResult;
  using namespace geelen_idf::mode_source;
  TEST_ASSERT_EQUAL_INT((int)ModeTransitionResult::Noop,
                        (int)geelen_idf::decideTransition(kBluetooth, kBluetooth));
  TEST_ASSERT_EQUAL_INT((int)ModeTransitionResult::Noop,
                        (int)geelen_idf::decideTransition(kSnapclient, kSnapclient));
  TEST_ASSERT_EQUAL_INT((int)ModeTransitionResult::Noop,
                        (int)geelen_idf::decideTransition(kDlna, kDlna));
  TEST_ASSERT_EQUAL_INT((int)ModeTransitionResult::Noop,
                        (int)geelen_idf::decideTransition(kTestTone, kTestTone));
}

void test_mode_fsm_bluetooth_either_side_reboots(void) {
  using geelen_idf::ModeTransitionResult;
  using namespace geelen_idf::mode_source;
  /* Any transition that crosses Bluetooth must reboot until Bluedroid
   * de-init becomes safe at runtime. */
  for (uint8_t other : {kSnapclient, kDlna, kTestTone}) {
    TEST_ASSERT_EQUAL_INT((int)ModeTransitionResult::RebootRequired,
                          (int)geelen_idf::decideTransition(kBluetooth, other));
    TEST_ASSERT_EQUAL_INT((int)ModeTransitionResult::RebootRequired,
                          (int)geelen_idf::decideTransition(other, kBluetooth));
  }
}

void test_mode_fsm_non_bluetooth_currently_reboots(void) {
  /* Today the non-Bluetooth diagonal also reboots because Snap / TestTone
   * lifecycles are owned by app_main rather than IProducer. The next
   * phase flips this branch to HotSwap; flipping the assertion is the
   * canary that the producer wiring landed. */
  using geelen_idf::ModeTransitionResult;
  using namespace geelen_idf::mode_source;
  for (uint8_t a : {kSnapclient, kDlna, kTestTone}) {
    for (uint8_t b : {kSnapclient, kDlna, kTestTone}) {
      if (a == b) continue;
      TEST_ASSERT_EQUAL_INT((int)ModeTransitionResult::RebootRequired,
                            (int)geelen_idf::decideTransition(a, b));
    }
  }
}

void test_mode_fsm_name_for_source(void) {
  using namespace geelen_idf::mode_source;
  TEST_ASSERT_EQUAL_STRING("Bluetooth",  geelen_idf::nameForSource(kBluetooth));
  TEST_ASSERT_EQUAL_STRING("Snapclient", geelen_idf::nameForSource(kSnapclient));
  TEST_ASSERT_EQUAL_STRING("DLNA",       geelen_idf::nameForSource(kDlna));
  TEST_ASSERT_EQUAL_STRING("TestTone",   geelen_idf::nameForSource(kTestTone));
  TEST_ASSERT_EQUAL_STRING("?",          geelen_idf::nameForSource(99));
}

void test_event_payload_sizes(void) {
  /* Pin sizes so a future struct change is visible. The DLNA URI is the
   * largest payload by far; keeping it ≤ 256 bytes bounds the per-post
   * memcpy cost on the producer side and matches the rest of the firmware
   * which uses 256-char URI buffers (see s_current_uri in idf_dlna.cpp). */
  TEST_ASSERT_EQUAL_size_t(4, sizeof(ev_wifi_up_t));
  TEST_ASSERT_EQUAL_size_t(1, sizeof(ev_wifi_down_t));
  TEST_ASSERT_EQUAL_size_t(2, sizeof(ev_mode_change_t));
  TEST_ASSERT_EQUAL_size_t(2, sizeof(ev_voice_prompt_t));
  TEST_ASSERT_EQUAL_size_t(256, sizeof(ev_dlna_play_t));
  TEST_ASSERT_EQUAL_size_t(8, sizeof(ev_state_diff_t));
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_gainDbToLinear_zero);
  RUN_TEST(test_gainDbToLinear_negative_40);
  RUN_TEST(test_gainDbToLinear_negative_12);
  RUN_TEST(test_gainDbToLinear_plus_6);
  RUN_TEST(test_parseSnapcastBaseHeader_too_short);
  RUN_TEST(test_parseSnapcastBaseHeader_exact_26);
  RUN_TEST(test_parseSnapcastBaseHeader_large_payload);
  RUN_TEST(test_buildSnapcastHelloFrame_roundtrip);
  RUN_TEST(test_buildSnapcastHelloFrame_out_too_small);
  RUN_TEST(test_buildSnapcastHelloFrame_null_inputs);
  RUN_TEST(test_eqBuildStageList_bell_bypass_zero_gain);
  RUN_TEST(test_eqBuildStageList_bell_three_db_one_stage);
  RUN_TEST(test_eqBuildStageList_lowpass_24_two_biquads);
  RUN_TEST(test_eqBuildStageList_lowpass_6_one_pole);
  RUN_TEST(test_eqLinearMagProduct_bell_boost_at_centre);
  RUN_TEST(test_event_ids_are_stable);
  RUN_TEST(test_event_payloads_are_pod);
  RUN_TEST(test_event_payload_sizes);
  RUN_TEST(test_mode_fsm_noop_for_same_source);
  RUN_TEST(test_mode_fsm_bluetooth_either_side_reboots);
  RUN_TEST(test_mode_fsm_non_bluetooth_currently_reboots);
  RUN_TEST(test_mode_fsm_name_for_source);
  return UNITY_END();
}
