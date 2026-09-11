find_package(Qt6 6.5 REQUIRED COMPONENTS Network Widgets)

qt_standard_project_setup()

qt_add_executable(rufus_plus_plus
  main.cpp
  main_window.cpp
  main_window.hpp
  secure_boot_assets.cpp
  secure_boot_assets.hpp
  uefi_shell_assets.cpp
  uefi_shell_assets.hpp
  windows_user_experience_dialog.cpp
  windows_user_experience_dialog.hpp
  resources.qrc
)

set_target_properties(rufus_plus_plus PROPERTIES
  AUTORCC TRUE
  OUTPUT_NAME "Rufus++"
  MACOSX_BUNDLE TRUE
  MACOSX_BUNDLE_BUNDLE_NAME "Rufus++"
  MACOSX_BUNDLE_BUNDLE_VERSION "${RUFUSPP_NUMERIC_BUILD_VERSION}"
  MACOSX_BUNDLE_SHORT_VERSION_STRING "${RUFUSPP_NUMERIC_BUILD_VERSION}"
  WIN32_EXECUTABLE TRUE
)

if(APPLE)
  set_target_properties(rufus_plus_plus PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER "${RUFUSPP_MACOS_APP_IDENTIFIER}"
  )
  if(RUFUSPP_MACOS_UNSIGNED_ROOT_MODE)
    add_custom_command(TARGET rufus_plus_plus POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E rm -f
              "$<TARGET_BUNDLE_DIR:rufus_plus_plus>/Contents/Resources/rufus-plus-plus-privileged-helper"
      COMMAND ${CMAKE_COMMAND} -E rm -f
              "$<TARGET_BUNDLE_DIR:rufus_plus_plus>/Contents/Library/LaunchDaemons/${RUFUSPP_MACOS_HELPER_IDENTIFIER}.plist"
      VERBATIM
    )
  else()
    add_dependencies(rufus_plus_plus rufus_plus_plus_privileged_helper)
    add_custom_command(TARGET rufus_plus_plus POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory
              "$<TARGET_BUNDLE_DIR:rufus_plus_plus>/Contents/Resources"
      COMMAND ${CMAKE_COMMAND} -E make_directory
              "$<TARGET_BUNDLE_DIR:rufus_plus_plus>/Contents/Library/LaunchDaemons"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "$<TARGET_FILE:rufus_plus_plus_privileged_helper>"
              "$<TARGET_BUNDLE_DIR:rufus_plus_plus>/Contents/Resources/rufus-plus-plus-privileged-helper"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${CMAKE_CURRENT_BINARY_DIR}/../platform/${RUFUSPP_MACOS_HELPER_IDENTIFIER}.plist"
              "$<TARGET_BUNDLE_DIR:rufus_plus_plus>/Contents/Library/LaunchDaemons/${RUFUSPP_MACOS_HELPER_IDENTIFIER}.plist"
      VERBATIM
    )

    if(RUFUSPP_MACOS_CODE_SIGN_IDENTITY)
      find_program(CODESIGN_EXECUTABLE codesign REQUIRED)
      add_custom_command(TARGET rufus_plus_plus POST_BUILD
        COMMAND ${CODESIGN_EXECUTABLE} --force --options runtime --timestamp
                --identifier "${RUFUSPP_MACOS_HELPER_IDENTIFIER}"
                --sign "${RUFUSPP_MACOS_CODE_SIGN_IDENTITY}"
                "$<TARGET_BUNDLE_DIR:rufus_plus_plus>/Contents/Resources/rufus-plus-plus-privileged-helper"
        COMMAND ${CODESIGN_EXECUTABLE} --force --options runtime --timestamp
                --identifier "${RUFUSPP_MACOS_APP_IDENTIFIER}"
                --sign "${RUFUSPP_MACOS_CODE_SIGN_IDENTITY}"
                "$<TARGET_BUNDLE_DIR:rufus_plus_plus>"
        VERBATIM
      )
    endif()
  endif()
endif()

target_compile_features(rufus_plus_plus PRIVATE cxx_std_17)
target_link_libraries(rufus_plus_plus PRIVATE
  RufusPlusPlus::PlatformBackend
  RufusPlusPlus::PortableCore
  Qt6::Network
  Qt6::Widgets
)

if(MSVC)
  target_compile_options(rufus_plus_plus PRIVATE /W4)
  if(RUFUSPP_STRICT_WARNINGS)
    target_compile_options(rufus_plus_plus PRIVATE /WX)
  endif()
else()
  target_compile_options(rufus_plus_plus PRIVATE -Wall -Wextra -Wpedantic)
  if(RUFUSPP_STRICT_WARNINGS)
    target_compile_options(rufus_plus_plus PRIVATE -Werror)
  endif()
endif()

install(TARGETS rufus_plus_plus
  BUNDLE DESTINATION .
  RUNTIME DESTINATION bin
)
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE.txt" DESTINATION .)

qt_generate_deploy_app_script(
  TARGET rufus_plus_plus
  OUTPUT_SCRIPT rufus_plus_plus_deploy_script
  NO_TRANSLATIONS
  NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT "${rufus_plus_plus_deploy_script}")

if(BUILD_TESTING)
  add_test(NAME qt-ui-smoke COMMAND rufus_plus_plus --smoke-test)
  set_tests_properties(qt-ui-smoke PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    TIMEOUT 15
  )
  add_test(NAME qt-uefi-shell-assets COMMAND rufus_plus_plus --verify-embedded-assets)
  set_tests_properties(qt-uefi-shell-assets PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    TIMEOUT 15
  )
  add_test(NAME qt-adaptive-window COMMAND rufus_plus_plus --verify-window-sizing)
  set_tests_properties(qt-adaptive-window PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    TIMEOUT 15
  )
  if(APPLE)
    if(RUFUSPP_MACOS_UNSIGNED_ROOT_MODE)
      add_test(
        NAME macos-unsigned-root-bundle
        COMMAND ${CMAKE_COMMAND}
                "-DAPP_BUNDLE=$<TARGET_BUNDLE_DIR:rufus_plus_plus>"
                "-DHELPER_IDENTIFIER=${RUFUSPP_MACOS_HELPER_IDENTIFIER}"
                -P "${PROJECT_SOURCE_DIR}/tests/verify_macos_unsigned_root_bundle.cmake"
      )
    else()
      add_test(
        NAME macos-helper-bundle
        COMMAND ${CMAKE_COMMAND}
                "-DAPP_BUNDLE=$<TARGET_BUNDLE_DIR:rufus_plus_plus>"
                "-DAPP_IDENTIFIER=${RUFUSPP_MACOS_APP_IDENTIFIER}"
                "-DHELPER_IDENTIFIER=${RUFUSPP_MACOS_HELPER_IDENTIFIER}"
                -P "${PROJECT_SOURCE_DIR}/tests/verify_macos_helper_bundle.cmake"
      )
    endif()
  endif()
endif()
