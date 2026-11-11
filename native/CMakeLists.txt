cmake_minimum_required(VERSION 3.18)
project(PoisonNative LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Managed entry point that the loader invokes once the IL2CPP runtime is ready.
# Must be a public static, parameterless method (see Plugin.cs -> Plugin.Bootstrap).
set(POISON_NAMESPACE "Poison" CACHE STRING "Namespace of the managed entry class")
set(POISON_CLASS     "Plugin" CACHE STRING "Managed entry class")
set(POISON_METHOD    "Bootstrap" CACHE STRING "Managed entry method (static, no args)")

add_library(Poison SHARED src/poison_loader.cpp)
set_target_properties(Poison PROPERTIES OUTPUT_NAME "Poison") # -> libPoison.so

target_compile_definitions(Poison PRIVATE
    POISON_NAMESPACE="${POISON_NAMESPACE}"
    POISON_CLASS="${POISON_CLASS}"
    POISON_METHOD="${POISON_METHOD}")

target_compile_options(Poison PRIVATE -Wall -Wextra -O2 -fno-exceptions -fno-rtti)

if(ANDROID)
    target_link_libraries(Poison PRIVATE log dl)
    # 16 KB page alignment (Android 15+), no build-id noise, drop unused sections.
    target_link_options(Poison PRIVATE
        -Wl,-z,max-page-size=16384
        -Wl,--gc-sections
        -Wl,--build-id=none)
else()
    # Host build is only used for the loader test (native/test).
    target_link_libraries(Poison PRIVATE dl pthread)
endif()
