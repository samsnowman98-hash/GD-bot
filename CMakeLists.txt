cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if ("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" OR IOS)
    set(CMAKE_OSX_ARCHITECTURES "arm64")
else()
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()

set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(Template VERSION 1.0.0)

# Рекурсивно собираем исходники
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)

# Создаем библиотеку
add_library(${PROJECT_NAME} SHARED ${SOURCES})

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode")
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

# Настройка зависимостей и Geode
setup_geode_mod(${PROJECT_NAME})

# ==============================================================================
# ФИКС БАГА БИНДИНГОВ GEODE 2.2081:
# Создаем фикс-хедер: подключаем DefaultInclude (пространство gd), затем SequenceTriggerState
# ==============================================================================
set(GEODE_FIX_HEADER "${CMAKE_CURRENT_BINARY_DIR}/geode_fix.hpp")
file(WRITE "${GEODE_FIX_HEADER}"
    "// Fix for Geode bindings codegen bug in GD 2.2081\n"
    "#pragma once\n"
    "#include <Geode/DefaultInclude.hpp>\n"
    "#include <Geode/binding/SequenceTriggerState.hpp>\n"
)

if (MSVC)
    file(TO_NATIVE_PATH "${GEODE_FIX_HEADER}" GEODE_FIX_HEADER_NATIVE)
    if (TARGET GeodeBindings)
        target_compile_options(GeodeBindings PRIVATE "/FI${GEODE_FIX_HEADER_NATIVE}")
    endif()
    if (TARGET ${PROJECT_NAME})
        target_compile_options(${PROJECT_NAME} PRIVATE "/FI${GEODE_FIX_HEADER_NATIVE}")
    endif()
else()
    if (TARGET GeodeBindings)
        target_compile_options(GeodeBindings PRIVATE "-include" "${GEODE_FIX_HEADER}")
    endif()
    if (TARGET ${PROJECT_NAME})
        target_compile_options(${PROJECT_NAME} PRIVATE "-include" "${GEODE_FIX_HEADER}")
    endif()
endif()
