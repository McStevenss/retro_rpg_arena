cmake_minimum_required(VERSION 3.22)
project(boyzpvp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Lets old third-party CMakeLists (ENet) configure on newer CMake versions.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# Windows: link the C/C++ runtime statically so the .exe files run without extra DLLs.
# (Must be set before the dependencies below are configured.)
if(MINGW)
  add_link_options(-static -static-libgcc -static-libstdc++)
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)

# --- Dependencies -----------------------------------------------------------
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG release-3.2.10
  GIT_SHALLOW TRUE)

FetchContent_Declare(enet
  GIT_REPOSITORY https://github.com/lsalzman/enet.git
  GIT_TAG v1.3.18
  GIT_SHALLOW TRUE)

FetchContent_Declare(glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG 1.0.1
  GIT_SHALLOW TRUE)

FetchContent_MakeAvailable(SDL3 enet glm)
target_include_directories(enet PUBLIC ${enet_SOURCE_DIR}/include)
if(WIN32)
  # Plain signature: ENet's own CMakeLists already uses it (and only links these on MinGW).
  target_link_libraries(enet ws2_32 winmm)
endif()

find_package(OpenGL REQUIRED)

if(WIN32)
  # windows.h (pulled in by enet.h) otherwise defines min/max macros that break std::min.
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(MSVC)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
  set(WARN /W4)
else()
  set(WARN -Wall -Wextra)
endif()

# --- Shared game code (used by both client and server) ----------------------
add_library(boyz_shared STATIC
  src/shared/game.cpp
  src/shared/map.cpp)
target_include_directories(boyz_shared PUBLIC src/shared)
target_compile_definitions(boyz_shared PRIVATE BOYZ_SOURCE_MAPS_DIR="${CMAKE_SOURCE_DIR}/maps")
target_compile_options(boyz_shared PRIVATE ${WARN})

# --- Dedicated server (headless, no SDL) ------------------------------------
add_executable(boyz_server src/server/main.cpp)
target_link_libraries(boyz_server PRIVATE boyz_shared enet)
target_compile_options(boyz_server PRIVATE ${WARN})

# --- Client -----------------------------------------------------------------
add_executable(boyz_client
  src/client/main.cpp
  src/client/gl.cpp
  src/client/render.cpp
  src/client/textures.cpp
  src/client/font.cpp)
target_link_libraries(boyz_client PRIVATE boyz_shared enet SDL3::SDL3 glm::glm OpenGL::GL)
target_compile_options(boyz_client PRIVATE ${WARN})

# Copy maps next to the executables so the build folder can be zipped and shared.
add_custom_target(copy_maps ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/maps $<TARGET_FILE_DIR:boyz_server>/maps)
