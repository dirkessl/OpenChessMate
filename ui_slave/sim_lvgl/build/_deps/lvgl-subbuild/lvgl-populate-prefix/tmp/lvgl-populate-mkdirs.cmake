# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/Users/essl.dirk/Nextcloud/CloudDrive/development/OpenChess/ui_slave/sim_lvgl/build/_deps/lvgl-src")
  file(MAKE_DIRECTORY "/Users/essl.dirk/Nextcloud/CloudDrive/development/OpenChess/ui_slave/sim_lvgl/build/_deps/lvgl-src")
endif()
file(MAKE_DIRECTORY
  "/Users/essl.dirk/Nextcloud/CloudDrive/development/OpenChess/ui_slave/sim_lvgl/build/_deps/lvgl-build"
  "/Users/essl.dirk/Nextcloud/CloudDrive/development/OpenChess/ui_slave/sim_lvgl/build/_deps/lvgl-subbuild/lvgl-populate-prefix"
  "/Users/essl.dirk/Nextcloud/CloudDrive/development/OpenChess/ui_slave/sim_lvgl/build/_deps/lvgl-subbuild/lvgl-populate-prefix/tmp"
  "/Users/essl.dirk/Nextcloud/CloudDrive/development/OpenChess/ui_slave/sim_lvgl/build/_deps/lvgl-subbuild/lvgl-populate-prefix/src/lvgl-populate-stamp"
  "/Users/essl.dirk/Nextcloud/CloudDrive/development/OpenChess/ui_slave/sim_lvgl/build/_deps/lvgl-subbuild/lvgl-populate-prefix/src"
  "/Users/essl.dirk/Nextcloud/CloudDrive/development/OpenChess/ui_slave/sim_lvgl/build/_deps/lvgl-subbuild/lvgl-populate-prefix/src/lvgl-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/essl.dirk/Nextcloud/CloudDrive/development/OpenChess/ui_slave/sim_lvgl/build/_deps/lvgl-subbuild/lvgl-populate-prefix/src/lvgl-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/essl.dirk/Nextcloud/CloudDrive/development/OpenChess/ui_slave/sim_lvgl/build/_deps/lvgl-subbuild/lvgl-populate-prefix/src/lvgl-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
