idf_component_register(
  SRCS
    "geelen_spotify_link.c"
  INCLUDE_DIRS
    "."
  REQUIRES
    esp_timer
)

# Configure bell/cspot to build minimal codec set for ESP32.
set(BELL_CODEC_AAC OFF CACHE BOOL "" FORCE)
set(BELL_CODEC_MP3 ON CACHE BOOL "" FORCE)
set(BELL_CODEC_OPUS OFF CACHE BOOL "" FORCE)
set(BELL_CODEC_ALAC OFF CACHE BOOL "" FORCE)
set(BELL_CODEC_VORBIS ON CACHE BOOL "" FORCE)
set(BELL_DISABLE_MQTT ON CACHE BOOL "" FORCE)
set(BELL_DISABLE_WEBSERVER ON CACHE BOOL "" FORCE)
set(BELL_ONLY_CJSON ON CACHE BOOL "" FORCE)

set(CSPOT_USE_PREBUILT_NANOPB ON CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../third_party/cspot/cspot" "${CMAKE_CURRENT_BINARY_DIR}/cspot_build")
target_link_libraries(${COMPONENT_LIB} PUBLIC cspot)
