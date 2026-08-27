/**
 * Native unit tests for TAS5825M Q1.31 conversion.
 * Run with: pio test -e native
 */

#include <unity.h>
#include "eq_q131.h"
#include <climits>
#include <cmath>

void test_q131_zero(void) {
  TEST_ASSERT_EQUAL_INT32(0, geelen::eqFloatToQ1_31(0.f));
}

void test_q131_one_saturates(void) {
  TEST_ASSERT_EQUAL_INT32(INT32_MAX, geelen::eqFloatToQ1_31(1.f));
}

void test_q131_neg_one_saturates(void) {
  TEST_ASSERT_EQUAL_INT32(INT32_MIN, geelen::eqFloatToQ1_31(-1.f));
}

void test_q131_half(void) {
  TEST_ASSERT_EQUAL_INT32(1 << 30, geelen::eqFloatToQ1_31(0.5f));
}

void test_q131_neg_half(void) {
  TEST_ASSERT_EQUAL_INT32(-(1 << 30), geelen::eqFloatToQ1_31(-0.5f));
}

void test_q131_nan_is_zero(void) {
  TEST_ASSERT_EQUAL_INT32(0, geelen::eqFloatToQ1_31(NAN));
}

void test_q131_roundtrip_quarter(void) {
  const int32_t q = geelen::eqFloatToQ1_31(0.25f);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.25f, geelen::eqQ1_31ToFloat(q));
}

void test_q131_identity_biquad_b0(void) {
  /* Identity biquad uses b0 = +1.0 → INT32_MAX (saturated Q1.31). */
  TEST_ASSERT_EQUAL_INT32(INT32_MAX, geelen::eqFloatToQ1_31(1.0f));
}
