cmake_minimum_required(VERSION 3.24)

project(
  RufusPlusPlus
  VERSION 0.1.0
  DESCRIPTION "A cross-platform boot-media utility"
  LANGUAGES CXX
)

if(APPLE)
  enable_language(OBJCXX)
  set(RUFUSPP_MACOS_APP_IDENTIFIER "org.rufusplusplus.app" CACHE STRING
      "Bundle identifier for the signed macOS application")
  set(RUFUSPP_MACOS_HELPER_IDENTIFIER "org.rufusplusplus.app.privileged-helper" CACHE STRING
      "Identifier and Mach service name for the macOS privileged helper")
  set(RUFUSPP_MACOS_TEAM_IDENTIFIER "" CACHE STRING
      "Apple Developer Team ID used to mutually authenticate the app and helper")
  set(RUFUSPP_MACOS_CODE_SIGN_IDENTITY "" CACHE STRING
      "Optional codesign identity used to sign the helper and app after bundling")
  foreach(identifier IN ITEMS RUFUSPP_MACOS_APP_IDENTIFIER RUFUSPP_MACOS_HELPER_IDENTIFIER)
    if(NOT "${${identifier}}" MATCHES "^[A-Za-z0-9.-]+$")
      message(FATAL_ERROR "${identifier} contains characters that are unsafe in a bundle ID")
    endif()
  endforeach()
  if(RUFUSPP_MACOS_TEAM_IDENTIFIER AND
     NOT RUFUSPP_MACOS_TEAM_IDENTIFIER MATCHES "^[A-Z0-9]+$")
    message(FATAL_ERROR "RUFUSPP_MACOS_TEAM_IDENTIFIER must be an Apple Team ID")
  endif()
endif()

include(CTest)

option(RUFUSPP_BUILD_QT_APP "Build the cross-platform Qt application" ON)
option(RUFUSPP_BUILD_PLATFORM_BACKENDS "Build native platform backends" ON)
option(RUFUSPP_STRICT_WARNINGS "Treat warnings in new cross-platform code as errors" OFF)

add_subdirectory(src/portable)

if(RUFUSPP_BUILD_PLATFORM_BACKENDS)
  add_subdirectory(src/platform)
endif()

if(RUFUSPP_BUILD_QT_APP)
  if(NOT RUFUSPP_BUILD_PLATFORM_BACKENDS)
    message(FATAL_ERROR "The Qt application requires native platform backends")
  endif()
  add_subdirectory(src/qt)
endif()

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
