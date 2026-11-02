# Censr for OBS: the add-on inside OBS that plays Censr's cleaned sound in place of your own mic.
# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.16)
project(censr-obs VERSION 0.1.0 LANGUAGES C)

# Linux: the system's OBS development files. Windows (OBS_SDK): OBS's own headers and import
# libraries made from its released DLLs, as .github/workflows/obs-addon.yml sets them out.
set(OBS_SDK "" CACHE PATH "OBS's headers (include/) and import libraries (lib/), where there's no OBS build")
if(OBS_SDK)
  add_library(obs_sdk INTERFACE)
  target_include_directories(obs_sdk INTERFACE "${OBS_SDK}/include")
  target_link_libraries(obs_sdk INTERFACE "${OBS_SDK}/lib/obs.lib" "${OBS_SDK}/lib/obs-frontend-api.lib"
                                          "${OBS_SDK}/lib/w32-pthreads.lib")
  add_library(OBS::libobs ALIAS obs_sdk)
  add_library(OBS::obs-frontend-api ALIAS obs_sdk)
else()
  find_package(libobs REQUIRED)
  find_package(obs-frontend-api REQUIRED)
endif()

add_library(censr-obs MODULE src/censr-addon.c src/censr-link.c src/censr-hmac.c src/censr-studio.c
                             src/censr-picture.c)
target_link_libraries(censr-obs PRIVATE OBS::libobs OBS::obs-frontend-api)
if(WIN32)
  target_link_libraries(censr-obs PRIVATE ws2_32 bcrypt)
endif()
set_target_properties(censr-obs PROPERTIES PREFIX "" C_STANDARD 11)
if(NOT MSVC)
  target_compile_options(censr-obs PRIVATE -Wall -Wextra -Werror)
endif()
