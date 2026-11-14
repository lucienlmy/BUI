# bui-lua: standalone Lua 5.4 host and loadable module for BUI.
# Finds Lua 5.4 on the system or fetches it at configure time.
# SPDX-License-Identifier: GPL-2.0-or-later

include(FetchContent)

# LuaRocks (blenderui-<version>.rockspec) builds only the loadable module, against the Lua that runs
# the rock: LuaRocks passes LUA_INCLUDE_DIR (and LUA_LIBRARY, which Windows needs), and the rock
# is installed with LUADIR / LIBDIR. Nothing is fetched, and the bui-lua executable is not built.
option(BUI_LUA_ROCK "Build only the _bui module for LuaRocks, against the host Lua (LUA_INCLUDE_DIR, LUA_LIBRARY)" OFF)

if(BUI_LUA_ROCK)
  if(NOT LUA_INCLUDE_DIR)
    message(FATAL_ERROR "bui-lua: BUI_LUA_ROCK needs LUA_INCLUDE_DIR (LuaRocks passes it)")
  endif()
  # LuaRocks on Windows passes these with backslashes, which CMake would read as escapes
  file(TO_CMAKE_PATH "${LUA_INCLUDE_DIR}" _rock_lua_inc)
  add_library(bui_lua_sys INTERFACE)
  target_include_directories(bui_lua_sys INTERFACE "${_rock_lua_inc}")
  if(LUA_LIBRARY)
    file(TO_CMAKE_PATH "${LUA_LIBRARY}" _rock_lua_lib)
    target_link_libraries(bui_lua_sys INTERFACE "${_rock_lua_lib}")
  endif()
  set(LUA_TARGET bui_lua_sys)
else()
# Try system Lua 5.4 first, then fetch from lua.org
# Note: find_package(Lua) with "5.4" matches 5.4 or newer (QUIET skips the
# version check in older CMake). We verify the major.minor ourselves.
find_package(Lua 5.4 QUIET)

# Also try the lowercase form (some Homebrew setups provide it)
if(NOT Lua_FOUND)
  find_package(lua 5.4 QUIET CONFIG) # config packages only: FindLua.cmake is the "Lua" form above
endif()

if(Lua_FOUND AND Lua_VERSION VERSION_GREATER_EQUAL 5.4 AND Lua_VERSION VERSION_LESS 5.5)
  message(STATUS "bui-lua: using system Lua ${Lua_VERSION} (${Lua_INCLUDE_DIR})")
  # CMake's FindLua sets LUA_LIBRARY and LUA_INCLUDE_DIR but does not
  # define an imported target until CMake 3.24. Use the variables directly.
  if(NOT LUA_LIBRARY OR NOT LUA_INCLUDE_DIR)
    message(FATAL_ERROR "bui-lua: system Lua ${Lua_VERSION} found but LUA_LIBRARY/LUA_INCLUDE_DIR not set")
  endif()
  add_library(bui_lua_sys INTERFACE)
  target_include_directories(bui_lua_sys INTERFACE ${LUA_INCLUDE_DIR})
  target_link_libraries(bui_lua_sys INTERFACE ${LUA_LIBRARY})
  set(LUA_TARGET bui_lua_sys)
else()
  # Clear cached find results from previous runs so we always fetch
  unset(LUA_LIBRARY CACHE)
  unset(LUA_INCLUDE_DIR CACHE)
  # Fetch Lua 5.4.7 from lua.org and build from source
  FetchContent_Declare(
    lua-5.4.7
    URL      https://www.lua.org/ftp/lua-5.4.7.tar.gz
    URL_HASH SHA256=9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30
    SOURCE_SUBDIR src
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_MakeAvailable(lua-5.4.7)
  set(LUA_SRC_DIR ${lua-5.4.7_SOURCE_DIR}/src)
  message(STATUS "bui-lua: building Lua 5.4.7 from ${LUA_SRC_DIR}")

  set(LUA_SRC
    ${LUA_SRC_DIR}/lapi.c      ${LUA_SRC_DIR}/lauxlib.c
    ${LUA_SRC_DIR}/lbaselib.c   ${LUA_SRC_DIR}/lcode.c
    ${LUA_SRC_DIR}/lcorolib.c   ${LUA_SRC_DIR}/lctype.c
    ${LUA_SRC_DIR}/ldblib.c     ${LUA_SRC_DIR}/ldebug.c
    ${LUA_SRC_DIR}/ldo.c        ${LUA_SRC_DIR}/ldump.c
    ${LUA_SRC_DIR}/lfunc.c      ${LUA_SRC_DIR}/lgc.c
    ${LUA_SRC_DIR}/linit.c      ${LUA_SRC_DIR}/liolib.c
    ${LUA_SRC_DIR}/llex.c       ${LUA_SRC_DIR}/lmathlib.c
    ${LUA_SRC_DIR}/lmem.c       ${LUA_SRC_DIR}/loadlib.c
    ${LUA_SRC_DIR}/lobject.c    ${LUA_SRC_DIR}/lopcodes.c
    ${LUA_SRC_DIR}/loslib.c     ${LUA_SRC_DIR}/lparser.c
    ${LUA_SRC_DIR}/lstate.c     ${LUA_SRC_DIR}/lstring.c
    ${LUA_SRC_DIR}/lstrlib.c    ${LUA_SRC_DIR}/ltable.c
    ${LUA_SRC_DIR}/ltablib.c    ${LUA_SRC_DIR}/ltm.c
    ${LUA_SRC_DIR}/lundump.c    ${LUA_SRC_DIR}/lutf8lib.c
    ${LUA_SRC_DIR}/lvm.c        ${LUA_SRC_DIR}/lzio.c)

  add_library(bui_lua STATIC ${LUA_SRC})
  target_include_directories(bui_lua PUBLIC ${LUA_SRC_DIR})
  if(NOT WIN32)
    # lua.org's makefile defines these (LUA_USE_LINUX / LUA_USE_MACOSX): without LUA_USE_DLOPEN
    # package.loadlib, and so `require` of any C module, is disabled
    target_compile_definitions(bui_lua PRIVATE LUA_USE_POSIX LUA_USE_DLOPEN)
    target_link_libraries(bui_lua PUBLIC ${CMAKE_DL_LIBS})
  endif()
  if(MSVC)
    target_compile_options(bui_lua PRIVATE /W0)
  else()
    target_compile_options(bui_lua PRIVATE -w -fvisibility=hidden)
  endif()
  set(LUA_TARGET bui_lua)
endif()
endif() # BUI_LUA_ROCK

if(NOT BUI_LUA_ROCK)
# linenoise (vendored, single-file MIT library)
# (linenoise needs termios.h, so Windows gets a small fgets version of the calls the REPL uses)
if(WIN32)
  set(_linenoise_src ${CMAKE_CURRENT_SOURCE_DIR}/extern/linenoise/linenoise_win32.c)
else()
  set(_linenoise_src ${CMAKE_CURRENT_SOURCE_DIR}/extern/linenoise/linenoise.c)
endif()
add_library(linenoise STATIC ${_linenoise_src})
target_include_directories(linenoise PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/linenoise)
endif() # NOT BUI_LUA_ROCK

# Shared BUI binding sources
set(LUABUI_SRC
    src/bui_lua.c
    src/bui_node.c
    src/bui_context.c
    src/bui_window.c
    src/bui_image.c
    src/bui_anim.c)

# the module (its REPL-free code) doesn't use linenoise
set(LUABUI_LIBS ${LUA_TARGET} bui::kit bui::gpu bui::core bui_api)

# bui-lua executable
if(NOT BUI_LUA_ROCK)
  add_executable(bui-lua src/main.c ${LUABUI_SRC})
  target_link_libraries(bui-lua PRIVATE ${LUABUI_LIBS} linenoise)
  target_include_directories(bui-lua PRIVATE src)
endif()

# Copy the Lua wrapper alongside the C module in the build tree
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/bui.lua ${CMAKE_CURRENT_BINARY_DIR}/bui.lua COPYONLY)

# Loadable module (_bui.so / _bui.dll) for stock Lua / LuaJIT / LuaRocks
# The underscore avoids a name collision with the Lua wrapper script (bui.lua).
# require("bui") finds bui.lua, which requires("_bui") for the C bindings.
add_library(_bui MODULE ${LUABUI_SRC})
target_link_libraries(_bui PRIVATE ${LUABUI_LIBS})
target_include_directories(_bui PRIVATE src)
set_target_properties(_bui PROPERTIES MODULE_NAME "_bui")
if(WIN32) # MSVC and MinGW alike: _bui.dll, not lib_bui.dll
  set_target_properties(_bui PROPERTIES PREFIX "" SUFFIX ".dll")
  if(LUA_TARGET STREQUAL "bui_lua_sys")
    # a system Lua on Windows is a DLL, and its API is imported from it; the Lua built here is static
    target_compile_definitions(_bui PRIVATE LUA_BUILD_AS_DLL)
  endif()
elseif(APPLE)
  set_target_properties(_bui PROPERTIES PREFIX "" SUFFIX ".so")
  target_link_options(_bui PRIVATE -Wl,-undefined,dynamic_lookup)
else()
  set_target_properties(_bui PROPERTIES PREFIX "" SUFFIX ".so")
endif()

# LuaRocks installs the module and its wrapper where it says (cmake --install, or `luarocks make`)
if(BUI_LUA_ROCK)
  file(TO_CMAKE_PATH "${LIBDIR}" _rock_libdir)
  file(TO_CMAKE_PATH "${LUADIR}" _rock_luadir)
  install(TARGETS _bui LIBRARY DESTINATION "${_rock_libdir}" RUNTIME DESTINATION "${_rock_libdir}")
  install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/bui.lua DESTINATION "${_rock_luadir}")
endif()

if(BUI_BUILD_TESTS AND NOT BUI_LUA_ROCK)
  add_test(NAME bui_lua_basic
    COMMAND bui-lua ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_basic.lua
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  set_tests_properties(bui_lua_basic PROPERTIES TIMEOUT 30)

  add_test(NAME bui_lua_render
    COMMAND bui-lua ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_render.lua
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  set_tests_properties(bui_lua_render PROPERTIES TIMEOUT 30)

  add_test(NAME bui_lua_widgets
    COMMAND bui-lua ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_widgets.lua
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  set_tests_properties(bui_lua_widgets PROPERTIES TIMEOUT 30)

  add_test(NAME bui_lua_context
    COMMAND bui-lua ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_context.lua
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  set_tests_properties(bui_lua_context PROPERTIES TIMEOUT 30)

  add_test(NAME bui_lua_images
    COMMAND bui-lua ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_images.lua
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  set_tests_properties(bui_lua_images PROPERTIES TIMEOUT 30)

  add_test(NAME bui_lua_theme
    COMMAND bui-lua ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_theme.lua
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  set_tests_properties(bui_lua_theme PROPERTIES TIMEOUT 30)

  add_test(NAME bui_lua_anim
    COMMAND bui-lua ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_anim.lua
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  set_tests_properties(bui_lua_anim PROPERTIES TIMEOUT 30)

  # the loadable module: Lua looks for luaopen__bui in _bui.dll / _bui.so
  add_test(NAME bui_lua_module
    COMMAND bui-lua ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_module.lua $<TARGET_FILE:_bui>
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  set_tests_properties(bui_lua_module PROPERTIES TIMEOUT 30)

  add_test(NAME bui_lua_window
    COMMAND bui-lua ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_window.lua
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  set_tests_properties(bui_lua_window PROPERTIES TIMEOUT 30)

  add_test(NAME bui_lua_filebrowser
    COMMAND bui-lua ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_filebrowser.lua
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  set_tests_properties(bui_lua_filebrowser PROPERTIES TIMEOUT 30)
endif()
