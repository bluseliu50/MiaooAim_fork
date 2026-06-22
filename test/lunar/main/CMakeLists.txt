# Compile the production module under test (main/lunar.c) together with the
# Unity test cases. lunar.c is pure C (only <stdio.h>/<string.h>) so it builds
# unchanged on the Linux host target.
idf_component_register(
    SRCS "test_lunar.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../../main/lunar.c"
    INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../main"
    REQUIRES unity
)
