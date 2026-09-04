/**
 * Native unit tests for TAS5825M Q1.31 / Q5.27 conversion.
 * Run with: pio test -e native
 */

#include <unity.h>
#include "eq_q131.h"
#include "eq_coeffs.h"
#include "eq_types.h"
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

void test_q5_27_identity_biquad_b0(void) {
  TEST_ASSERT_EQUAL_INT32(0x08000000, geelen::eqFloatToQ5_27(1.0f));
}

void test_tas5825m_feedback_a1_a2_negated(void) {
  /* ESPHome tas58xx 20 Hz / -10 dB / Q=2 peaking @ 48 kHz stores
   * a1=0x0FFBC31A (~+2) and a2=0xF8043C00 (~-1). Textbook RBJ a1 is
   * ~-2; the TAS DSP adds a1*y1 so the upload must negate. */
  geelen::EqBand band{};
  band.freq_hz = 20.f;
  band.gain_db = -10.f;
  band.q = 2.f;
  band.type = geelen::EqFilterType::Bell;
  geelen::EqStage stages[geelen::EQ_MAX_STAGES_PER_LAYER];
  const size_t n = geelen::eqBuildStageList(48000.f, &band, 1, stages);
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)n);
  TEST_ASSERT_TRUE(stages[0].a1 < -1.5f);
  TEST_ASSERT_TRUE(stages[0].a2 > 0.5f);

  int32_t hw[5];
  geelen::eqStageToTas5825mQ5_27(stages[0].b0, stages[0].b1, stages[0].b2,
                                 stages[0].a1, stages[0].a2, hw);
  TEST_ASSERT_GREATER_THAN_INT32(0, hw[3]);
  TEST_ASSERT_LESS_THAN_INT32(0, hw[4]);
  TEST_ASSERT_INT32_WITHIN(0x20000, 0x0FFBC31A, hw[3]);
  TEST_ASSERT_INT32_WITHIN(0x20000, (int32_t)0xF8043C00, hw[4]);
}

void test_volume_pct_to_tas_mute(void) {
  TEST_ASSERT_EQUAL_UINT8(0xFF, geelen::eqVolumePctToTasDigVol(0));
}

void test_volume_pct_to_tas_unity(void) {
  TEST_ASSERT_EQUAL_UINT8(0x30, geelen::eqVolumePctToTasDigVol(100));
  TEST_ASSERT_EQUAL_UINT8(0x30, geelen::eqVolumePctToTasDigVol(101));
}

void test_volume_pct_to_tas_31_is_about_minus_10db(void) {
  /* 20*log10(0.31) ≈ −10.17 dB → 20 half-dB steps → 0x30+20 = 0x44.
   * The old linear map (100-pct)*2 put 31% at 0xBA (−69 dB). */
  TEST_ASSERT_EQUAL_UINT8(0x44, geelen::eqVolumePctToTasDigVol(31));
}

void test_volume_pct_to_tas_50_is_about_minus_6db(void) {
  /* 20*log10(0.5) ≈ −6.02 dB → 12 half-dB steps → 0x3C. */
  TEST_ASSERT_EQUAL_UINT8(0x3C, geelen::eqVolumePctToTasDigVol(50));
}
