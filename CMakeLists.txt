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

# ==============================================================================
# ПАТЧ TULIPHOOK (Фикс падения Windows-сборки):
# В AbstractType.hpp тернарный оператор форсирует вычисление sizeof() на типах
# по ссылке. Заменяем на `if constexpr`, чтобы отсечь неполные типы.
# ==============================================================================
file(GLOB_RECURSE TULIP_HEADERS
    "${CMAKE_SOURCE_DIR}/cpm-cache/*AbstractType.hpp"
    "${CMAKE_BINARY_DIR}/*AbstractType.hpp"
    "${CMAKE_CURRENT_BINARY_DIR}/*AbstractType.hpp"
)
if (DEFINED tuliphook_SOURCE_DIR)
    list(APPEND TULIP_HEADERS "${tuliphook_SOURCE_DIR}/include/tulip/AbstractType.hpp")
endif()
list(REMOVE_DUPLICATES TULIP_HEADERS)

foreach(HEADER_PATH ${TULIP_HEADERS})
    if (EXISTS "${HEADER_PATH}")
        file(READ "${HEADER_PATH}" HEADER_CONTENT)
        set(HEADER_MODIFIED FALSE)
        if (HEADER_CONTENT MATCHES "type\\.m_size = std::is_reference_v<Type> \\? sizeof\\(void\\*\\) : sizeof\\(Type\\);")
            string(REPLACE
                "type.m_size = std::is_reference_v<Type> ? sizeof(void*) : sizeof(Type);"
                "if constexpr (std::is_reference_v<Type>) type.m_size = sizeof(void*); else type.m_size = sizeof(Type);"
                HEADER_CONTENT "${HEADER_CONTENT}"
            )
            set(HEADER_MODIFIED TRUE)
        endif()
        if (HEADER_CONTENT MATCHES "type\\.m_alignment = std::is_reference_v<Type> \\? alignof\\(void\\*\\) : alignof\\(Type\\);")
            string(REPLACE
                "type.m_alignment = std::is_reference_v<Type> ? alignof(void*) : alignof(Type);"
                "if constexpr (std::is_reference_v<Type>) type.m_alignment = alignof(void*); else type.m_alignment = alignof(Type);"
                HEADER_CONTENT "${HEADER_CONTENT}"
            )
            set(HEADER_MODIFIED TRUE)
        endif()
        if (HEADER_MODIFIED)
            file(WRITE "${HEADER_PATH}" "${HEADER_CONTENT}")
            message(STATUS "Patched TulipHook bug in: ${HEADER_PATH}")
        endif()
    endif()
endforeach()

# Настройка зависимостей и Geode
setup_geode_mod(${PROJECT_NAME})

# ==============================================================================
# ФИКС БАГА БИНДИНГОВ GEODE 2.2081:
# Подключаем DefaultInclude, EffectManagerState и SequenceTriggerState
# ==============================================================================
set(GEODE_FIX_HEADER "${CMAKE_CURRENT_BINARY_DIR}/geode_fix.hpp")
file(WRITE "${GEODE_FIX_HEADER}"
    "// Fix for Geode bindings codegen bug in GD 2.2081\n"
    "#pragma once\n"
    "#include <Geode/DefaultInclude.hpp>\n"
    "#include <Geode/binding/EffectManagerState.hpp>\n"
    "#include <Geode/binding/SequenceTriggerState.hpp>\n"
)

if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    file(TO_NATIVE_PATH "${GEODE_FIX_HEADER}" GEODE_FIX_HEADER_NATIVE)
    set(FIX_COMPILE_FLAGS "/FI${GEODE_FIX_HEADER_NATIVE}")
else()
    set(FIX_COMPILE_FLAGS "-include" "${GEODE_FIX_HEADER}")
endif()

if (TARGET GeodeBindings)
    target_compile_options(GeodeBindings PRIVATE ${FIX_COMPILE_FLAGS})
endif()

if (TARGET ${PROJECT_NAME})
    target_compile_options(${PROJECT_NAME} PRIVATE ${FIX_COMPILE_FLAGS})
endif()
