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

# Добавляем все исходники из src рекурсивно
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)

# Создаем библиотеку мода
add_library(${PROJECT_NAME} SHARED ${SOURCES})

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode")
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

# Настройка зависимостей и линковка с Geode
setup_geode_mod(${PROJECT_NAME})

# ==============================================================================
# ФИКС ОШИБКИ: "field has incomplete type 'SequenceTriggerState'"
# В GD 2.2081 генератор биндингов Geode не инклудит SequenceTriggerState.hpp
# перед SequenceTriggerGameObject.hpp в unity-сборке.
# Данные флаги принудительно подключают объявление типа перед компиляцией.
# ==============================================================================
if (TARGET GeodeBindings)
    if (MSVC)
        target_compile_options(GeodeBindings PRIVATE /FI"Geode/binding/SequenceTriggerState.hpp")
    else()
        target_compile_options(GeodeBindings PRIVATE -include Geode/binding/SequenceTriggerState.hpp)
    endif()
endif()

if (TARGET ${PROJECT_NAME})
    if (MSVC)
        target_compile_options(${PROJECT_NAME} PRIVATE /FI"Geode/binding/SequenceTriggerState.hpp")
    else()
        target_compile_options(${PROJECT_NAME} PRIVATE -include Geode/binding/SequenceTriggerState.hpp)
    endif()
endif()
