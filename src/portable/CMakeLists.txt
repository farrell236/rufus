add_library(rufus_plus_plus_portable_core STATIC
  src/application_info.cpp
  src/bad_block_test.cpp
  src/compressed_image_inspector.cpp
  src/compressed_image_source.cpp
  src/deployment_quality.cpp
  src/ext2_formatter.cpp
  src/file_image_writer.cpp
  src/format.cpp
  src/image_analyzer.cpp
  src/image_profile.cpp
  src/iso_deployment.cpp
  src/iso9660_reader.cpp
  src/linux_persistence.cpp
  src/media.cpp
  src/macos_installer.cpp
  src/media_capture.cpp
  src/media_inspector.cpp
  src/platform.cpp
  src/raw_image_writer.cpp
  src/safety_policy.cpp
  src/secure_boot_analyzer.cpp
  src/udf_reader.cpp
  src/wim_splitter.cpp
  src/wim_applier.cpp
  src/write_plan.cpp
  src/wim_inspector.cpp
  src/windows_to_go.cpp
)

find_package(ZLIB REQUIRED)
find_package(BZip2 REQUIRED)
find_package(LibLZMA REQUIRED)
find_package(zstd CONFIG QUIET)
if(TARGET zstd::libzstd_static)
  set(RUFUSPP_ZSTD_TARGET zstd::libzstd_static)
elseif(TARGET zstd::libzstd)
  set(RUFUSPP_ZSTD_TARGET zstd::libzstd)
else()
  find_path(RUFUSPP_ZSTD_INCLUDE_DIR NAMES zstd.h)
  find_library(RUFUSPP_ZSTD_LIBRARY NAMES zstd libzstd)
  if(NOT RUFUSPP_ZSTD_INCLUDE_DIR OR NOT RUFUSPP_ZSTD_LIBRARY)
    message(FATAL_ERROR "Zstandard headers and library are required")
  endif()
  add_library(rufus_plus_plus_zstd UNKNOWN IMPORTED)
  set_target_properties(rufus_plus_plus_zstd PROPERTIES
    IMPORTED_LOCATION "${RUFUSPP_ZSTD_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${RUFUSPP_ZSTD_INCLUDE_DIR}"
  )
  set(RUFUSPP_ZSTD_TARGET rufus_plus_plus_zstd)
endif()

add_library(RufusPlusPlus::PortableCore ALIAS rufus_plus_plus_portable_core)

target_include_directories(rufus_plus_plus_portable_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(rufus_plus_plus_portable_core PUBLIC cxx_std_17)
target_compile_definitions(rufus_plus_plus_portable_core
  PUBLIC RUFUSPP_VERSION="${RUFUSPP_BUILD_VERSION}"
)
if(WIN32)
  target_compile_definitions(rufus_plus_plus_portable_core PRIVATE LZMA_API_STATIC)
endif()
target_link_libraries(rufus_plus_plus_portable_core PRIVATE
  ZLIB::ZLIB
  BZip2::BZip2
  LibLZMA::LibLZMA
  ${RUFUSPP_ZSTD_TARGET}
)

if(CMAKE_DL_LIBS)
  target_link_libraries(rufus_plus_plus_portable_core PRIVATE ${CMAKE_DL_LIBS})
endif()

if(MSVC)
  target_compile_options(rufus_plus_plus_portable_core PRIVATE /W4)
  if(RUFUSPP_STRICT_WARNINGS)
    target_compile_options(rufus_plus_plus_portable_core PRIVATE /WX)
  endif()
else()
  target_compile_options(rufus_plus_plus_portable_core PRIVATE -Wall -Wextra -Wpedantic)
  if(RUFUSPP_STRICT_WARNINGS)
    target_compile_options(rufus_plus_plus_portable_core PRIVATE -Werror)
  endif()
endif()
