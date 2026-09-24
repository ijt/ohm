cmake_minimum_required(VERSION 3.21)
project(ohm-browser VERSION 0.2.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets WebEngineWidgets Sql PrintSupport)

find_package(PkgConfig REQUIRED)
# Real Lua, not a hand-rolled parser -- ~/.config/ohm-browser/config.lua (see
# Config.h) is a real .lua file executed by this interpreter, so it can use
# actual Lua (variables, conditionals, etc.), not just flat key = "value"
# lines. Pinned to 5.4 (vs. the unversioned "lua" package, currently 5.5 on
# this machine) for a stable, well-known embedding API.
pkg_check_modules(LUA REQUIRED IMPORTED_TARGET lua5.4)

qt_add_executable(ohm-browser-bin
  src/main.cpp
  src/Notify.cpp
  src/DownloadManager.cpp
  src/DownloadBar.cpp
  src/DownloadsPanelLauncher.cpp
  src/WebProfile.cpp
  src/WebNotifications.cpp
  src/ThemeLoader.cpp
  src/Config.cpp
  src/HistoryStore.cpp
  src/UrlMatch.cpp
  src/PopularDomains.cpp
  src/OmniboxOverlay.cpp
  src/PasskeyBroker.cpp
  src/PasskeyOverlay.cpp
  src/PermissionBar.cpp
  src/FindBar.cpp
  src/ReadlineEditing.cpp
  src/BrowserWindow.cpp
  src/DevToolsWindow.cpp
  src/Hyprland.cpp
  src/SingletonServer.cpp
  src/SingletonClient.cpp
  resources/ohm.qrc
)

target_link_libraries(ohm-browser-bin PRIVATE
  Qt6::Widgets
  Qt6::WebEngineWidgets
  Qt6::Sql
  Qt6::PrintSupport
  PkgConfig::LUA
)

target_compile_definitions(ohm-browser-bin PRIVATE
  OHM_VERSION="${PROJECT_VERSION}")

# Unit tests: `ctest --test-dir app/build`. Packagers can pass
# -DBUILD_TESTING=OFF to skip building them.
include(CTest)
if(BUILD_TESTING)
  find_package(Qt6 REQUIRED COMPONENTS Test)
  qt_add_executable(url_match_test tests/UrlMatchTest.cpp src/UrlMatch.cpp)
  target_include_directories(url_match_test PRIVATE src)
  target_link_libraries(url_match_test PRIVATE Qt6::Core Qt6::Test)
  add_test(NAME url_match COMMAND url_match_test)
endif()

include(GNUInstallDirs)

# /usr/bin/ohm is the shell wrapper so `ohm uninstall` runs the
# command instead of the browser. The ELF stays ohm-browser-bin; naming it
# `ohm` made `ohm uninstall` a URL, which paints a blank window.
install(PROGRAMS "${CMAKE_CURRENT_SOURCE_DIR}/../ohm"
  DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS $<TARGET_FILE:ohm-browser-bin>
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/ohm-browser)

# Phone passkeys (../passkey-helper): a Rust helper next to ohm-browser-bin,
# where PasskeyBroker looks for it. Without cargo, Ohm builds and runs
# as before, just without passkeys. Cargo keeps its own incremental state,
# so the always-run target is a quick no-op when nothing changed.
option(OHM_PASSKEYS "Build ohm-browser-passkey for phone passkeys (needs cargo)" ON)
if(OHM_PASSKEYS)
  find_program(CARGO_EXECUTABLE cargo)
  if(CARGO_EXECUTABLE)
    set(_passkey_src "${CMAKE_CURRENT_SOURCE_DIR}/../passkey-helper")
    set(_passkey_bin "${CMAKE_CURRENT_BINARY_DIR}/ohm-browser-passkey")
    add_custom_target(ohm-browser-passkey ALL
      COMMAND "${CARGO_EXECUTABLE}" build --release --locked
              --manifest-path "${_passkey_src}/Cargo.toml"
              --target-dir "${CMAKE_CURRENT_BINARY_DIR}/cargo"
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different
              "${CMAKE_CURRENT_BINARY_DIR}/cargo/release/ohm-browser-passkey" "${_passkey_bin}"
      COMMENT "Building ohm-browser-passkey (phone passkeys)"
      USES_TERMINAL)
    install(PROGRAMS "${_passkey_bin}" DESTINATION ${CMAKE_INSTALL_LIBDIR}/ohm-browser)
  else()
    message(WARNING "cargo not found: building without phone passkeys (ohm-browser-passkey)")
  endif()
endif()

set(_ohm_packaging_dir "${CMAKE_CURRENT_SOURCE_DIR}/../packaging")
install(FILES "${_ohm_packaging_dir}/ohm-browser.desktop"
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.png"
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/128x128/apps
  RENAME ohm-browser.png)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.svg"
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps
  RENAME ohm-browser.svg)

# systemd user unit: ExecStart must match the install prefix (/usr for
# packages, /usr/local for a plain cmake --install). The daemon is the ELF,
# not the wrapper, so a packaged unit does not depend on the shell script.
set(OHM_DAEMON_EXEC "${CMAKE_INSTALL_FULL_LIBDIR}/ohm-browser/ohm-browser-bin")
configure_file("${_ohm_packaging_dir}/ohm-browser.service.in"
  "${CMAKE_CURRENT_BINARY_DIR}/ohm-browser.service" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/ohm-browser.service"
  DESTINATION "lib/systemd/user")
