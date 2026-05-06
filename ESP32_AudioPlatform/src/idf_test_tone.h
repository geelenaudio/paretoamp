#pragma once

#include <cstdint>

namespace geelen {
class AudioEngine;
}
namespace geelen_idf {
class ConfigManager;

/** Start Core-1 task feeding sine from `cfg` into `eng` (no-op if already running). */
void testToneStart(geelen::AudioEngine* eng, ConfigManager* cfg);
void testToneStop();
}  // namespace geelen_idf

/** C hook for idf_api (weak); strong definition in `idf_test_tone.cpp`. */
extern "C" void testToneNotifyParamsUpdated(void);
