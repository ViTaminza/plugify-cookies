# cookies — Plugify C++ plugin: per-player persistent cookies on top of plgdb.
#
#   cookies_core     engine-independent storage core (also used by tests/cookies_tests.cpp)
#   cookies_config   the databases.cfg reader (PLUGIN_STANDARD.md section 4)
#   plugify-cookies  the plugin shared library (entry "bin/cookies" -> bin/libcookies.so)

set(COOKIES_VERSION "1.0.2")

# --- core ---------------------------------------------------------------------
add_library(cookies_core STATIC src/cookie_store.cpp)
target_include_directories(cookies_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(cookies_core PUBLIC plgdb)
target_compile_options(cookies_core PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(cookies_core PUBLIC _GLIBCXX_USE_CXX11_ABI=1)

# --- database settings ---------------------------------------------------------
# The reader of <GetConfigsDir()>/cookies/databases.cfg (section "cookies"), on top of
# pisexcompat's KeyValues parser and path mapping. Separate from cookies_core so the storage core
# stays free of plugify/pisexcompat dependencies, and separate from plugin.cpp so the tests can
# drive the precedence rules without an engine.
add_library(cookies_config STATIC src/db_config.cpp)
target_include_directories(cookies_config PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the pisexcompat and generated sdk headers must not fail -Werror on our sources.
target_include_directories(cookies_config SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/libs/pisexcompat/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(cookies_config PUBLIC plgdb PRIVATE pisexcompat)
target_compile_options(cookies_config PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(cookies_config PUBLIC _GLIBCXX_USE_CXX11_ABI=1)

# --- plugin -------------------------------------------------------------------
add_library(plugify-cookies SHARED
        src/plugin.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
)
target_include_directories(plugify-cookies PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated s2sdk/plg and pisexcompat headers must not fail -Werror.
target_include_directories(plugify-cookies SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/libs/pisexcompat/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-cookies PRIVATE
        cookies_core
        cookies_config
        -static-libstdc++ -static-libgcc
)
target_compile_options(plugify-cookies PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-cookies PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="cookies"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-cookies PROPERTIES OUTPUT_NAME cookies)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-cookies PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cookies.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/cookies.pplugin"
)

# --- tests ---------------------------------------------------------------------
# Databases.cfg precedence and the transition-period fallback (tests/db_config_tests.cpp); run by
# ../test.sh from build/tests like every other *_tests executable.
add_executable(cookies_dbconfig_tests tests/db_config_tests.cpp)
target_include_directories(cookies_dbconfig_tests SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/libs/pisexcompat/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(cookies_dbconfig_tests PRIVATE cookies_config -static-libstdc++ -static-libgcc)
target_compile_options(cookies_dbconfig_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(cookies_dbconfig_tests PRIVATE _GLIBCXX_USE_CXX11_ABI=1)
set_target_properties(cookies_dbconfig_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
