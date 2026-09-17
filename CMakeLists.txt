cmake_minimum_required(VERSION 3.25)
project(spear VERSION 0.1.0 LANGUAGES CXX)

# S.P.E.A.R. — Signal Processing & Emission Analysis Receiver
# FZ-G2 / B210 専用。他PC・他SDRへの移植性は要件としない(要件 §1)。

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(SPEAR_TSAN        "Build with ThreadSanitizer (要件 §12.2: CIに常設)" OFF)
option(SPEAR_TAP         "Enable TAP() intermediate-stream dumps (要件 §5.3)" ON)
option(SPEAR_WITH_UHD    "Build B210 live source/sink (needs libuhd)" ON)
option(SPEAR_BUILD_TESTS "Build unit tests" ON)
option(SPEAR_BUILD_TOOLS "Build CLI tools (soak, headless)" ON)
option(SPEAR_BUILD_GUI   "Build Qt Quick GUI (要件 §9)" ON)

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)

if(SPEAR_TSAN)
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -O1 -g)
  add_link_options(-fsanitize=thread)
endif()

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFTW3F REQUIRED IMPORTED_TARGET fftw3f)
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)

if(SPEAR_WITH_UHD)
  find_package(UHD 4.0 REQUIRED)
endif()
find_package(onnxruntime CONFIG REQUIRED)   # STD-T98 秘話の hybrid モデル(要件 §3.4 道 2)。Arch: onnxruntime-cpu。最上位で探す(imported target をディレクトリ横断で見せる)

add_subdirectory(core)
add_subdirectory(dsp)

if(SPEAR_BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
endif()

if(SPEAR_BUILD_GUI)
  find_package(Qt6 6.6 REQUIRED COMPONENTS Core Gui Qml Quick GuiPrivate)
  qt_standard_project_setup(REQUIRES 6.6)
  add_subdirectory(theme)
  add_subdirectory(input)
  add_subdirectory(appfw)
  add_subdirectory(widgets)
  add_subdirectory(apps)
  add_subdirectory(gui)
endif()

if(SPEAR_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(SPEAR_BUILD_TESTS)
  add_subdirectory(tests)
endif()
