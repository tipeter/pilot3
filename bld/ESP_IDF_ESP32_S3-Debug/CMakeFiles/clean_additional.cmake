# Additional clean files
cmake_minimum_required(VERSION 3.16)

if("${CONFIG}" STREQUAL "" OR "${CONFIG}" STREQUAL "Debug")
  file(REMOVE_RECURSE
  "esp-idf/mbedtls/x509_crt_bundle"
  "esp32s3_pilot.bin"
  "esp32s3_pilot.map"
  "flash_app_args"
  "flash_bootloader_args"
  "flash_project_args"
  "flasher_args.json"
  "flasher_args.json.in"
  "ldgen_libraries"
  "ldgen_libraries.in"
  "littlefs_py_venv"
  "project_elf_src_esp32s3.c"
  "server.crt.S"
  "server.key.S"
  "web_ui.html.S"
  "x509_crt_bundle.S"
  )
endif()
