cmake_minimum_required(VERSION 3.21)
project(shinto-app VERSION 0.0.4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets WebEngineWidgets Sql PrintSupport)

find_package(PkgConfig REQUIRED)
# Real Lua, not a hand-rolled parser -- ~/.config/shinto/config.lua (see
# Config.h) is a real .lua file executed by this interpreter, so it can use
# actual Lua (variables, conditionals, etc.), not just flat key = "value"
# lines. Pinned to 5.4 (vs. the unversioned "lua" package, currently 5.5 on
# this machine) for a stable, well-known embedding API.
pkg_check_modules(LUA REQUIRED IMPORTED_TARGET lua5.4)

qt_add_executable(shinto-bin
  src/main.cpp
  src/Notify.cpp
  src/DownloadManager.cpp
  src/DownloadBar.cpp
  src/DownloadsPanelLauncher.cpp
  src/WebProfile.cpp
  src/ThemeLoader.cpp
  src/Config.cpp
  src/HistoryStore.cpp
  src/PopularDomains.cpp
  src/OmniboxOverlay.cpp
  src/FindBar.cpp
  src/ReadlineEditing.cpp
  src/BrowserWindow.cpp
  src/Hyprland.cpp
  src/SingletonServer.cpp
  src/SingletonClient.cpp
  resources/shinto.qrc
)

target_link_libraries(shinto-bin PRIVATE
  Qt6::Widgets
  Qt6::WebEngineWidgets
  Qt6::Sql
  Qt6::PrintSupport
  PkgConfig::LUA
)

target_compile_definitions(shinto-bin PRIVATE
  SHINTO_VERSION="${PROJECT_VERSION}")

include(GNUInstallDirs)

# /usr/bin/shinto is the shell wrapper so `shinto uninstall` runs the
# command instead of the browser. The ELF stays shinto-bin; naming it
# `shinto` made `shinto uninstall` a URL, which paints a blank window.
install(PROGRAMS "${CMAKE_CURRENT_SOURCE_DIR}/../shinto"
  DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS $<TARGET_FILE:shinto-bin>
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/shinto)

set(_shinto_packaging_dir "${CMAKE_CURRENT_SOURCE_DIR}/../packaging")
install(FILES "${_shinto_packaging_dir}/shinto.desktop"
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.png"
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/128x128/apps
  RENAME shinto.png)

# systemd user unit: ExecStart must match the install prefix (/usr for
# packages, /usr/local for a plain cmake --install). The daemon is the ELF,
# not the wrapper, so a packaged unit does not depend on the shell script.
set(SHINTO_DAEMON_EXEC "${CMAKE_INSTALL_FULL_LIBDIR}/shinto/shinto-bin")
configure_file("${_shinto_packaging_dir}/shinto.service.in"
  "${CMAKE_CURRENT_BINARY_DIR}/shinto.service" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/shinto.service"
  DESTINATION "lib/systemd/user")
