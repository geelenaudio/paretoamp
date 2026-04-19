/**
 * Native unit tests – audio_util (gainDbToLinear) and snapcast_protocol (parseSnapcastBaseHeader).
 * Run with: pio test -e native
 */

#include <unity.h>
#include "audio_util.h"
#include "snapcast_protocol.h"
#include <cstring>
#include <cmath>

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
  return UNITY_END();
}
