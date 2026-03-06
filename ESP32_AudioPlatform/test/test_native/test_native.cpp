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
  return UNITY_END();
}
