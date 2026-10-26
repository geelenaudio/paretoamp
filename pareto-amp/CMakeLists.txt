cmake_minimum_required(VERSION 3.16)

# S3-only extras (USB Audio Class gadget). Must be set before project.cmake.
# WROVER USB is a UART bridge; do not discover this tree on esp32.
if("${IDF_TARGET}" STREQUAL "esp32s3")
  list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components_s3")
endif()

# ESP-IDF project boilerplate (required by PlatformIO when framework=espidf).
# PROJECT_VER must be set before include(project.cmake) so it lands in
# esp_app_desc.version (About / GET /api/system). Source of truth is
# version.txt; release CI overwrites it from the git tag.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/version.txt")
  file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/version.txt" PROJECT_VER LIMIT_COUNT 1)
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pareto_amp)

