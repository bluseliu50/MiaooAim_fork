cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "2.3.3")

# Fresh source archives do not include the local sdkconfig file. Without an
# explicit target, ESP-IDF defaults to esp32 and then S3-only GPIO/USB headers
# fail later during compilation.
if(NOT DEFINED IDF_TARGET)
    set(IDF_TARGET "esp32s3")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(epaper_uploader)

set(FONTFS_DIR "${PROJECT_DIR}/spiffs_image")
set(OPEN_FONT_DIR "${PROJECT_DIR}/tools/fonts/noto")
set(OPEN_FONT_READY "${OPEN_FONT_DIR}/.fonts-ready")
set(FONTFS_FONT16 "${FONTFS_DIR}/fonts/cjk16.mef")
set(FONTFS_FONT24 "${FONTFS_DIR}/fonts/cjk24.mef")
set(FONTFS_FONT32 "${FONTFS_DIR}/fonts/cjk32.mef")
idf_build_get_property(python PYTHON)
add_custom_command(
    OUTPUT "${OPEN_FONT_READY}"
    COMMAND ${python} "${PROJECT_DIR}/tools/fetch_open_fonts.py"
            --out-dir "${OPEN_FONT_DIR}"
    DEPENDS
        "${PROJECT_DIR}/tools/fetch_open_fonts.py"
    WORKING_DIRECTORY "${PROJECT_DIR}"
    COMMENT "Fetching OFL-licensed fonts for bitmap generation"
)
add_custom_command(
    OUTPUT "${FONTFS_FONT16}" "${FONTFS_FONT24}" "${FONTFS_FONT32}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${FONTFS_DIR}/fonts"
    COMMAND ${CMAKE_COMMAND} -E rm -f "${FONTFS_DIR}/fonts/cjk48.mef"
    COMMAND ${python} "${PROJECT_DIR}/tools/gen_ext_font.py"
            --size 16
            --out "${FONTFS_FONT16}"
            --set gb2312
            --require-open-fonts
            --threshold 104
            --cjk-stroke 0
            --quiet
    COMMAND ${python} "${PROJECT_DIR}/tools/gen_ext_font.py"
            --size 24
            --out "${FONTFS_FONT24}"
            --set gb2312
            --require-open-fonts
            --threshold 110
            --cjk-stroke 0
            --quiet
    COMMAND ${python} "${PROJECT_DIR}/tools/gen_ext_font.py"
            --size 32
            --out "${FONTFS_FONT32}"
            --set gb2312
            --require-open-fonts
            --threshold 118
            --cjk-stroke 0
            --quiet
    DEPENDS
        "${OPEN_FONT_READY}"
        "${PROJECT_DIR}/tools/gen_ext_font.py"
        "${PROJECT_DIR}/tools/gen_font.py"
        "${PROJECT_DIR}/tools/font_extra_chars.txt"
    WORKING_DIRECTORY "${PROJECT_DIR}"
    COMMENT "Generating built-in 16/24/32px LXGW 975 Yuan CJK fonts for fontfs"
)
add_custom_target(fontfs_font DEPENDS
    "${FONTFS_FONT16}"
    "${FONTFS_FONT24}"
    "${FONTFS_FONT32}"
)
spiffs_create_partition_image(fontfs "${FONTFS_DIR}"
    FLASH_IN_PROJECT
    DEPENDS fontfs_font
)
