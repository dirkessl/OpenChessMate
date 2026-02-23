cmake_minimum_required(VERSION 3.16)
project(ui_slave_lvgl C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

include(FetchContent)

FetchContent_Declare(
  lvgl
  GIT_REPOSITORY https://github.com/lvgl/lvgl.git
  GIT_TAG v8.3.0
)
## Ensure our local include dir (with lv_conf.h) is visible to the lvgl build
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# Provide LVGL config macro globally so the dependency build picks up lv_conf.h
add_compile_definitions(LV_CONF_INCLUDE_SIMPLE)

FetchContent_MakeAvailable(lvgl)

find_package(SDL2 REQUIRED)

# Ensure SDL2 include paths are correctly set for macOS Homebrew installations
if(NOT DEFINED SDL2_INCLUDE_DIRS)
  find_path(SDL2_INCLUDE_DIR SDL.h
    PATHS
      /opt/homebrew/opt/sdl2/include
      /opt/homebrew/include/SDL2
      /usr/local/include/SDL2
    NO_DEFAULT_PATH
  )
  if(SDL2_INCLUDE_DIR)
    set(SDL2_INCLUDE_DIRS ${SDL2_INCLUDE_DIR})
  else()
    message(FATAL_ERROR "SDL2 headers not found. Please install SDL2 using Homebrew.")
  endif()
endif()

## Make sure SDL2 include dirs are visible. `find_package(SDL2)` may set
## `SDL2_INCLUDE_DIRS` or provide `SDL2::SDL2`. Add common Homebrew paths as
## fallback so the compile can find <SDL2/SDL.h> on macOS.
if(DEFINED SDL2_INCLUDE_DIRS)
  set(_sdl2_includes ${SDL2_INCLUDE_DIRS})
else()
  set(_sdl2_includes "/opt/homebrew/opt/sdl2/include" "/opt/homebrew/include/SDL2" "/usr/local/include/SDL2")
endif()

file(GLOB PIECE_SRCS ../src/pieces/*.c)
add_executable(ui_slave_lvgl main.cpp ../src/chess_ui.cpp ../src/fonts/open_chess_font_32.c ${PIECE_SRCS})
target_include_directories(ui_slave_lvgl PRIVATE ${lvgl_SOURCE_DIR} ../src ../src/fonts ../src/pieces ${CMAKE_CURRENT_SOURCE_DIR}/include ${SDL2_INCLUDE_DIRS})
target_compile_definitions(ui_slave_lvgl PRIVATE LV_CONF_INCLUDE_SIMPLE)
target_link_libraries(ui_slave_lvgl PRIVATE lvgl SDL2::SDL2)
