add_executable(rufus_plus_plus_portable_core_tests
  portable_core_tests.cpp
)

target_compile_features(rufus_plus_plus_portable_core_tests PRIVATE cxx_std_17)
target_link_libraries(rufus_plus_plus_portable_core_tests PRIVATE RufusPlusPlus::PortableCore)

add_test(NAME portable-core COMMAND rufus_plus_plus_portable_core_tests)

add_executable(rufus_plus_plus_backend_core_tests
  backend_core_tests.cpp
)

target_compile_features(rufus_plus_plus_backend_core_tests PRIVATE cxx_std_17)
target_include_directories(rufus_plus_plus_backend_core_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/src/portable/src
)
target_link_libraries(rufus_plus_plus_backend_core_tests PRIVATE RufusPlusPlus::PortableCore)

add_test(NAME backend-core COMMAND rufus_plus_plus_backend_core_tests)

if(TARGET rufus_plus_plus_platform_backend)
  add_executable(rufus_plus_plus_platform_backend_tests
    platform_backend_tests.cpp
  )

  target_compile_features(rufus_plus_plus_platform_backend_tests PRIVATE cxx_std_17)
  target_link_libraries(rufus_plus_plus_platform_backend_tests PRIVATE RufusPlusPlus::PlatformBackend)

  add_test(NAME platform-backend COMMAND rufus_plus_plus_platform_backend_tests)
endif()
