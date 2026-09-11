if(NOT DEFINED APP_BUNDLE OR NOT DEFINED APP_IDENTIFIER OR NOT DEFINED HELPER_IDENTIFIER)
  message(FATAL_ERROR "macOS helper bundle test is missing configuration")
endif()

set(helper "${APP_BUNDLE}/Contents/Resources/rufus-plus-plus-privileged-helper")
set(plist "${APP_BUNDLE}/Contents/Library/LaunchDaemons/${HELPER_IDENTIFIER}.plist")
if(NOT EXISTS "${helper}")
  message(FATAL_ERROR "Bundled privileged helper is missing: ${helper}")
endif()
if(NOT EXISTS "${plist}")
  message(FATAL_ERROR "Bundled launch-daemon plist is missing: ${plist}")
endif()

execute_process(
  COMMAND /usr/bin/plutil -lint "${plist}"
  RESULT_VARIABLE plist_result
  OUTPUT_QUIET
  ERROR_VARIABLE plist_error
)
if(NOT plist_result EQUAL 0)
  message(FATAL_ERROR "Bundled launch-daemon plist is invalid: ${plist_error}")
endif()

file(READ "${plist}" plist_contents)
foreach(required IN ITEMS
    "<string>${HELPER_IDENTIFIER}</string>"
    "<string>${APP_IDENTIFIER}</string>"
    "<string>Contents/Resources/rufus-plus-plus-privileged-helper</string>")
  string(FIND "${plist_contents}" "${required}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "Bundled launch-daemon plist is missing ${required}")
  endif()
endforeach()
