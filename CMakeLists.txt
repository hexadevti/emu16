# CMakeLists.txt -- DESKTOP (SDL2) debug build of emu16. The arduino-cli device build never runs
# CMake and never defines BOARD_DESKTOP.
#
#   Windows, MSYS2 MinGW32 shell (or the "desktop: build" VS Code task):
#     cmake -G "Unix Makefiles" -S . -B build -DCMAKE_BUILD_TYPE=Debug && make -C build -j4
#   Linux:
#     cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build
cmake_minimum_required(VERSION 3.16)
project(emu16_desktop C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()
# The cores are far too slow at -O0 to judge sound and speed; keep debug info, optimise a little.
set(CMAKE_C_FLAGS_DEBUG "-Og -g3 -fno-omit-frame-pointer")
set(CMAKE_CXX_FLAGS_DEBUG "-Og -g3 -fno-omit-frame-pointer")

set(REPO ${CMAKE_CURRENT_SOURCE_DIR})
set(SKETCH ${REPO}/emu16)

# Match the ESP32's unsigned `char`.
add_compile_options(-funsigned-char)

option(EMU_M32 "Build 32-bit (-m32), like the ESP32-P4's ILP32" OFF)
if(EMU_M32)
  add_compile_options(-m32)
  add_link_options(-m32)
endif()

if(EMU_SANITIZE)
  add_compile_options(-fsanitize=${EMU_SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${EMU_SANITIZE})
endif()

# Every source under src/ except the P4 HAL (those files are #if-guarded too).
file(GLOB_RECURSE EMU_SRC CONFIGURE_DEPENDS ${SKETCH}/src/*.c ${SKETCH}/src/*.cpp)
list(FILTER EMU_SRC EXCLUDE REGEX "/src/hal/picocalc/")

configure_file(${SKETCH}/emu16.ino ${CMAKE_BINARY_DIR}/emu16_ino.cpp COPYONLY)

find_package(SDL2 REQUIRED)

add_executable(emu16 ${CMAKE_BINARY_DIR}/emu16_ino.cpp ${EMU_SRC})
target_include_directories(emu16 PRIVATE ${SKETCH})
target_compile_definitions(emu16 PRIVATE BOARD_DESKTOP SDL_MAIN_HANDLED)
option(EMU_PORTABLE "Use sdcard/ next to the executable instead of the repo's sdcard/" OFF)
if(NOT EMU_PORTABLE)
  target_compile_definitions(emu16 PRIVATE EMU_DEFAULT_SD_DIR="${REPO}/sdcard")
endif()
# Gwenesis is C written for an embedded compiler; keep its warnings out of the way.
set_source_files_properties(${EMU_SRC} PROPERTIES COMPILE_OPTIONS "$<$<COMPILE_LANGUAGE:C>:-w>")

if(TARGET SDL2::SDL2)
  target_link_libraries(emu16 PRIVATE SDL2::SDL2)
else()
  target_include_directories(emu16 PRIVATE ${SDL2_INCLUDE_DIRS})
  target_link_libraries(emu16 PRIVATE ${SDL2_LIBRARIES})
endif()

# Copy the MinGW + SDL2 runtime DLLs next to the exe so it runs without msys64 on PATH.
if(WIN32)
  get_filename_component(MINGW_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
  foreach(dll SDL2.dll libstdc++-6.dll libwinpthread-1.dll
              libgcc_s_dw2-1.dll libgcc_s_sjlj-1.dll libgcc_s_seh-1.dll)
    if(EXISTS "${MINGW_BIN}/${dll}")
      add_custom_command(TARGET emu16 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MINGW_BIN}/${dll}" "$<TARGET_FILE_DIR:emu16>"
        VERBATIM)
    endif()
  endforeach()
endif()
