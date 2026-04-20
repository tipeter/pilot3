# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/ptihanyi/esp32/esp-idf/components/bootloader/subproject"
  "/home/ptihanyi/esp32/Projects/pilot3/bld/ESP_IDF_ESP32_S3-Debug/bootloader"
  "/home/ptihanyi/esp32/Projects/pilot3/bld/ESP_IDF_ESP32_S3-Debug/bootloader-prefix"
  "/home/ptihanyi/esp32/Projects/pilot3/bld/ESP_IDF_ESP32_S3-Debug/bootloader-prefix/tmp"
  "/home/ptihanyi/esp32/Projects/pilot3/bld/ESP_IDF_ESP32_S3-Debug/bootloader-prefix/src/bootloader-stamp"
  "/home/ptihanyi/esp32/Projects/pilot3/bld/ESP_IDF_ESP32_S3-Debug/bootloader-prefix/src"
  "/home/ptihanyi/esp32/Projects/pilot3/bld/ESP_IDF_ESP32_S3-Debug/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/ptihanyi/esp32/Projects/pilot3/bld/ESP_IDF_ESP32_S3-Debug/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/ptihanyi/esp32/Projects/pilot3/bld/ESP_IDF_ESP32_S3-Debug/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
