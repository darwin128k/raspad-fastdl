cmake_minimum_required(VERSION 3.16)
project(fastdl C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "FastDL targets Linux HLDS")
endif()

option(FASTDL_BUILD_MODULE "Build the i386 Authum module" ON)
option(FASTDL_SANITIZE "Enable ASan/UBSan" OFF)
set(AUTHUM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/authum" CACHE PATH "Authum source tree")
find_package(Threads REQUIRED)

# Production module: configuration, source adaptation and host API calls only.
add_library(fastdl_logic STATIC
    src/fastdl/config.c
    src/fastdl/http.c
)
target_include_directories(fastdl_logic PUBLIC include "${AUTHUM_DIR}/include")
target_compile_definitions(fastdl_logic PUBLIC _FILE_OFFSET_BITS=64)
set_target_properties(fastdl_logic PROPERTIES POSITION_INDEPENDENT_CODE ON)

function(fastdl_configure_code target)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Werror
        -fvisibility=hidden -fstack-protector-strong
        -ffunction-sections -fdata-sections
        $<$<CONFIG:Release>:-Os>
    )
endfunction()
fastdl_configure_code(fastdl_logic)

if(FASTDL_BUILD_MODULE)
    target_compile_options(fastdl_logic PUBLIC -m32)
    target_link_options(fastdl_logic PUBLIC -m32)
    add_subdirectory("${AUTHUM_DIR}/client" "${CMAKE_BINARY_DIR}/authum-client")
    target_link_libraries(fastdl_logic PUBLIC authum_client)

    add_library(fastdl MODULE
        src/authum/info.c
        src/authum/module.c
        src/authum/bridge.c
    )
    fastdl_configure_code(fastdl)
    target_link_options(fastdl PRIVATE
        -static-libgcc "LINKER:--gc-sections" "LINKER:-z,relro,-z,now"
    )
    target_link_libraries(fastdl PRIVATE fastdl_logic)
    set_target_properties(fastdl PROPERTIES
        PREFIX ""
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/modules/fastdl"
    )
    if(CMAKE_BUILD_TYPE MATCHES "^(Release|MinSizeRel)$" AND CMAKE_STRIP
       AND NOT FASTDL_SANITIZE)
        add_custom_command(TARGET fastdl POST_BUILD
            COMMAND ${CMAKE_STRIP} --strip-unneeded $<TARGET_FILE:fastdl>
        )
    endif()
    install(TARGETS fastdl
        LIBRARY DESTINATION addons/authum/modules/fastdl COMPONENT fastdl
    )
    install(FILES "${AUTHUM_DIR}/LICENSE"
        DESTINATION addons/authum/modules/fastdl/licenses
        RENAME authum-LICENSE COMPONENT fastdl
    )
    install(FILES "${AUTHUM_DIR}/lib/lh/LICENSE.md"
        DESTINATION addons/authum/modules/fastdl/licenses
        RENAME lh-LICENSE.md COMPONENT fastdl
    )
else()
    # Native sanitizer tests need SDK dispatchers, not a second full host build.
    add_library(fastdl_test_client STATIC
        "${AUTHUM_DIR}/src/authum/api/thread.c"
        "${AUTHUM_DIR}/src/authum/api/http.c"
        "${AUTHUM_DIR}/src/authum/api/clock.c"
        "${AUTHUM_DIR}/src/authum/api/placeholders.c"
        "${AUTHUM_DIR}/src/authum/api/file.c"
        "${AUTHUM_DIR}/src/authum/api/memory.c"
        "${AUTHUM_DIR}/src/authum/api/cstr.c"
        "${AUTHUM_DIR}/src/authum/api/path.c"
    )
    target_include_directories(fastdl_test_client PUBLIC "${AUTHUM_DIR}/include")
    target_link_libraries(fastdl_logic PUBLIC fastdl_test_client)
endif()

if(FASTDL_SANITIZE)
    target_compile_options(fastdl_logic PUBLIC
        -fsanitize=address,undefined -fno-omit-frame-pointer
    )
    target_link_options(fastdl_logic PUBLIC -fsanitize=address,undefined)
endif()

install(FILES fastdl.cfg
    DESTINATION addons/authum/modules/fastdl/configs COMPONENT fastdl
)
install(FILES LICENSE
    DESTINATION addons/authum/modules/fastdl/licenses RENAME fastdl-LICENSE COMPONENT fastdl
)

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    # Private host-backend tests use LH parsing and OS primitives through the
    # host implementation. The module consumes only Authum's public service.
    if(NOT TARGET lh)
        set(LH_BUILD_TESTS OFF CACHE BOOL "Build LH tests" FORCE)
        set(LH_BUILD_DOCS OFF CACHE BOOL "Build LH docs" FORCE)
        set(LH_BUILD_BENCH OFF CACHE BOOL "Build LH benchmarks" FORCE)
        set(LH_LIBRARY_OPTION_OS ON CACHE BOOL "Host tests need LH OS services" FORCE)
        add_subdirectory("${AUTHUM_DIR}/lib/lh" "${CMAKE_BINARY_DIR}/lh" EXCLUDE_FROM_ALL)
    endif()
    if(TARGET fastdl_test_client)
        # The SDK dispatchers use lh's headers and its runtime allocator.
        target_link_libraries(fastdl_test_client PUBLIC lh)
    endif()
    file(GLOB AUTHUM_HTTP_TEST_SOURCES CONFIGURE_DEPENDS
        "${AUTHUM_DIR}/src/authum/host/http/*.c"
    )
    # Same host implementation as authum.so, never linked into fastdl.so.
    add_library(fastdl_test_host STATIC
        "${AUTHUM_DIR}/src/authum/host/threads.c"
        "${AUTHUM_DIR}/src/authum/host/clock.c"
        "${AUTHUM_DIR}/src/authum/host/placeholders.c"
        ${AUTHUM_HTTP_TEST_SOURCES}
    )
    target_include_directories(fastdl_test_host
        PUBLIC "${AUTHUM_DIR}/include" "${AUTHUM_DIR}/src"
    )
    target_compile_definitions(fastdl_test_host PUBLIC _FILE_OFFSET_BITS=64)
    # The SDK's own host sources are held to the same bar, minus the lh header
    # macro that trips -Wtype-limits on unsigned comparisons.
    target_compile_options(fastdl_test_host PRIVATE -Wall -Wextra -Werror -Wno-type-limits)
    target_link_libraries(fastdl_test_host PUBLIC Threads::Threads lh)
    if(FASTDL_BUILD_MODULE)
        target_compile_options(fastdl_test_host PUBLIC -m32)
        target_link_options(fastdl_test_host PUBLIC -m32)
    endif()
    if(FASTDL_SANITIZE)
        target_compile_options(fastdl_test_host PUBLIC
            -fsanitize=address,undefined -fno-omit-frame-pointer
        )
        target_link_options(fastdl_test_host PUBLIC -fsanitize=address,undefined)
    endif()

    add_executable(fastdl_test_http tests/http_driver.c)
    target_compile_options(fastdl_test_http PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(fastdl_test_http PRIVATE fastdl_logic fastdl_test_host)
    add_test(NAME fastdl_http COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/http.py $<TARGET_FILE:fastdl_test_http>
    )
    set_tests_properties(fastdl_http PROPERTIES TIMEOUT 45)

    if(FASTDL_BUILD_MODULE)
        add_executable(fastdl_test_module tests/module.c)
        target_compile_options(fastdl_test_module PRIVATE -Wall -Wextra -Werror)
        target_link_libraries(fastdl_test_module PRIVATE fastdl_logic fastdl_test_host dl)
        add_test(NAME fastdl_module COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/module.py
            $<TARGET_FILE:fastdl_test_module> $<TARGET_FILE:fastdl>
        )
        set_tests_properties(fastdl_module PROPERTIES TIMEOUT 40)
    endif()
endif()

