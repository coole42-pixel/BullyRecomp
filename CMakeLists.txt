# BullyRecomp - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(BullyRecomp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(BULLYRECOMP_SOURCES
    src/main.cpp
    src/audio_compat.cpp
    src/guest_locks.cpp
    src/autoplay.cpp
    src/settings_ini.cpp
)

if(WIN32)
    add_executable(BullyRecomp WIN32 ${BULLYRECOMP_SOURCES})
else()
    add_executable(BullyRecomp ${BULLYRECOMP_SOURCES})
endif()

rexglue_setup_target(BullyRecomp GPU_PLUGINS xenos)
if(WIN32)
    target_link_libraries(BullyRecomp PRIVATE winmm psapi)
    # Linker map lets tools/resolve-profile.py turn sampled addresses into function names.
    target_link_options(BullyRecomp PRIVATE -Xlinker /MAP:${CMAKE_BINARY_DIR}/BullyRecomp.map)
endif()

# Use runtime DLLs built from a local rexglue-sdk checkout (same commit as the installed SDK,
# so headers and import libraries still match) instead of the prebuilt ones. The copy runs
# after rexglue_setup_target's own staging step, so the local DLLs win.
set(BULLYRECOMP_RUNTIME_OVERRIDE_DIR "" CACHE PATH
    "Directory with locally built rexruntime.dll / rexgpu-xenos.dll to stage instead of the SDK's")
if(BULLYRECOMP_RUNTIME_OVERRIDE_DIR)
    foreach(dll rexruntime.dll rexgpu-xenos.dll)
        if(EXISTS "${BULLYRECOMP_RUNTIME_OVERRIDE_DIR}/${dll}")
            add_custom_command(TARGET BullyRecomp POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${BULLYRECOMP_RUNTIME_OVERRIDE_DIR}/${dll}" "$<TARGET_FILE_DIR:BullyRecomp>"
                COMMENT "Staging local ${dll}")
            message(STATUS "Runtime override: ${BULLYRECOMP_RUNTIME_OVERRIDE_DIR}/${dll}")
        endif()
    endforeach()
endif()

option(BULLYRECOMP_GPU_DIAGNOSTICS "Build optional GPU draw inspection backend" OFF)
if(BULLYRECOMP_GPU_DIAGNOSTICS)
    target_sources(BullyRecomp PRIVATE src/gpu_diagnostics.cpp)
    target_compile_definitions(BullyRecomp PRIVATE BULLYRECOMP_GPU_DIAGNOSTICS=1)
    target_link_libraries(BullyRecomp PRIVATE rex::gpu-xenos)
endif()

option(BULLYRECOMP_BUILD_BRANCH_TESTS "Build focused original-XEX branch checks" OFF)
if(BULLYRECOMP_BUILD_BRANCH_TESTS)
    enable_testing()
    add_executable(BullyRecomp_branch_tests tools/branch-tests.cpp src/audio_compat.cpp)
    target_include_directories(BullyRecomp_branch_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(BullyRecomp_branch_tests PRIVATE BullyRecomp_recomp rex::runtime)
    rexglue_apply_target_settings(BullyRecomp_branch_tests)
    add_test(NAME repaired_branches COMMAND BullyRecomp_branch_tests ${CMAKE_CURRENT_SOURCE_DIR}/assets)
    set_tests_properties(repaired_branches PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:rex::rexglue>")
endif()
