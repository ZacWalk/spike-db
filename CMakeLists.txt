cmake_minimum_required(VERSION 3.20)
project(spike_db LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# gnu11, not c11: the POSIX path uses fdatasync, nanosleep and F_OFD_SETLKW,
# which strict ISO mode hides.
set(CMAKE_C_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(SPIKEDB_AVX512   "Target AVX-512 instead of AVX2"          OFF)
option(SPIKEDB_AUDIT    "Compile in the structural audit hook"    OFF)
option(SPIKEDB_SANITIZE "Build with AddressSanitizer (+UBSan)"    OFF)
set(SPIKEDB_TEST_CACHE "" CACHE STRING "Page-cache size for the test binary")

# ---------------------------------------------------------------- library ---
add_library(spike_db STATIC src/spike_db.c)
target_include_directories(spike_db PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(MSVC)
    target_compile_options(spike_db PUBLIC /W4)
    if(SPIKEDB_AVX512)
        target_compile_options(spike_db PUBLIC /arch:AVX512)
        target_compile_definitions(spike_db PUBLIC _SPIKEDB_COMPILE_AVX512)
    else()
        target_compile_options(spike_db PUBLIC /arch:AVX2)
    endif()
else()
    target_compile_options(spike_db PUBLIC -Wall -Wextra -msse4.2)
    if(SPIKEDB_AVX512)
        target_compile_options(spike_db PUBLIC -mavx2 -mavx512f)
        target_compile_definitions(spike_db PUBLIC _SPIKEDB_COMPILE_AVX512)
    else()
        target_compile_options(spike_db PUBLIC -mavx2)
    endif()
endif()

if(SPIKEDB_AUDIT)
    target_compile_definitions(spike_db PUBLIC SPIKEDB_AUDIT)
endif()

if(SPIKEDB_SANITIZE)
    if(MSVC)
        # CMake's Debug defaults enable runtime checks incompatible with ASan.
        string(REGEX REPLACE "/RTC[1su]+" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
        target_compile_options(spike_db PUBLIC /fsanitize=address)
        target_link_options(spike_db PUBLIC /INCREMENTAL:NO)
    else()
        target_compile_options(spike_db PUBLIC
            -fsanitize=address,undefined -fno-sanitize-recover=undefined
            -fno-omit-frame-pointer -g)
        target_link_options(spike_db PUBLIC -fsanitize=address,undefined)
    endif()
endif()

# ------------------------------------------------------------------- tests ---
# CTest uses a per-build tmp/ so different variants never share test databases.
add_executable(test_spike_db src/test_spike_db.c)
target_link_libraries(test_spike_db PRIVATE spike_db)

if(SPIKEDB_TEST_CACHE)
    target_compile_definitions(test_spike_db PRIVATE TEST_CACHE=${SPIKEDB_TEST_CACHE}u)
endif()

if(MSVC)
    target_compile_definitions(test_spike_db PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

include(CTest)
if(BUILD_TESTING)
    add_test(NAME suite
             COMMAND test_spike_db
             WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(suite PROPERTIES LABELS spike_db TIMEOUT 120)
    if(SPIKEDB_SANITIZE AND NOT MSVC)
        add_executable(ubsan_failure_probe tests/ubsan-failure.c)
        target_link_libraries(ubsan_failure_probe PRIVATE spike_db)
        add_test(NAME ubsan_failure
                 COMMAND "${CMAKE_COMMAND}"
                         "-DPROBE=$<TARGET_FILE:ubsan_failure_probe>"
                         "-DTEST_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/tmp/ubsan-failure/$<CONFIG>"
                         -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/ubsan-failure.cmake")
        set_tests_properties(ubsan_failure PROPERTIES LABELS spike_db TIMEOUT 30)
    endif()
    find_program(POWERSHELL_EXECUTABLE NAMES pwsh)
    if(POWERSHELL_EXECUTABLE)
        add_test(NAME dd_adoption
                 COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -File
                         "${CMAKE_CURRENT_SOURCE_DIR}/tests/dd-adoption.ps1")
        set_tests_properties(dd_adoption PROPERTIES LABELS adoption TIMEOUT 120)
    else()
        message(STATUS "PowerShell not found; skipping dd adoption test (C suite remains enabled)")
    endif()
endif()
