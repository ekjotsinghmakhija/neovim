add_library(main_lib INTERFACE)
add_executable(nvim main.c)

add_library(libuv INTERFACE)
find_package(libuv CONFIG)
if(TARGET libuv::uv_a)
  target_link_libraries(libuv INTERFACE libuv::uv_a)
  mark_as_advanced(libuv_DIR)
else()
  # Fall back to find module for older libuv versions that don't provide config file
  find_package(Libuv 1.28.0 REQUIRED MODULE)
  target_include_directories(libuv SYSTEM BEFORE INTERFACE ${LIBUV_INCLUDE_DIRS})
  target_link_libraries(libuv INTERFACE ${LIBUV_LIBRARIES})
endif()

find_package(Libluv 1.43.0 REQUIRED)
target_include_directories(main_lib SYSTEM BEFORE INTERFACE ${LIBLUV_INCLUDE_DIR})
target_link_libraries(main_lib INTERFACE ${LIBLUV_LIBRARY})

find_package(Iconv REQUIRED)
find_package(Libtermkey 0.22 REQUIRED)
find_package(Libvterm 0.3 REQUIRED)
find_package(Msgpack 1.0.0 REQUIRED)
find_package(Treesitter 0.20.8 REQUIRED)
find_package(Unibilium 2.0 REQUIRED)

target_link_libraries(main_lib INTERFACE
  iconv
  libtermkey
  libvterm
  msgpack
  treesitter
  unibilium)

# Libintl (not Intl) selects our FindLibintl.cmake script. #8464
find_package(Libintl REQUIRED)
target_include_directories(main_lib SYSTEM BEFORE INTERFACE ${LIBINTL_INCLUDE_DIR})
if (LIBINTL_LIBRARY)
  target_link_libraries(main_lib INTERFACE ${LIBINTL_LIBRARY})
endif()

# The unit test lib requires LuaJIT; it will be skipped if LuaJIT is missing.
option(PREFER_LUA "Prefer Lua over LuaJIT in the nvim executable." OFF)
if(PREFER_LUA)
  find_package(Lua 5.1 EXACT REQUIRED)
  target_include_directories(main_lib SYSTEM BEFORE INTERFACE ${LUA_INCLUDE_DIR})
  target_link_libraries(main_lib INTERFACE ${LUA_LIBRARIES})
  # Passive (not REQUIRED): if LUAJIT_FOUND is not set, fixtures for unittests is skipped.
  find_package(Luajit)
else()
  find_package(Luajit REQUIRED)
  target_include_directories(main_lib SYSTEM BEFORE INTERFACE ${LUAJIT_INCLUDE_DIRS})
  target_link_libraries(main_lib INTERFACE ${LUAJIT_LIBRARIES})
endif()

option(ENABLE_IWYU "Run include-what-you-use with the compiler." OFF)
if(ENABLE_IWYU)
  find_program(IWYU_PRG NAMES include-what-you-use iwyu)
  if(NOT IWYU_PRG)
    message(FATAL_ERROR "ENABLE_IWYU is ON but include-what-you-use is not found!")
  endif()

  set(iwyu_flags "${IWYU_PRG};")
  string(APPEND iwyu_flags "-Xiwyu;--no_default_mappings;")
  string(APPEND iwyu_flags "-Xiwyu;--mapping_file=${PROJECT_SOURCE_DIR}/cmake.config/iwyu/mapping.imp;")
  string(APPEND iwyu_flags "-Xiwyu;--mapping_file=${PROJECT_SOURCE_DIR}/cmake.config/iwyu/gcc.libc.imp;")
  string(APPEND iwyu_flags "-Xiwyu;--mapping_file=${PROJECT_SOURCE_DIR}/cmake.config/iwyu/gcc.symbols.imp")

  set_target_properties(nvim PROPERTIES C_INCLUDE_WHAT_YOU_USE "${iwyu_flags}")
  target_compile_definitions(main_lib INTERFACE EXITFREE)
endif()

if(MSVC)
  target_compile_options(main_lib INTERFACE -W3)

  # Disable warnings that give too many false positives.
  target_compile_options(main_lib INTERFACE -wd4311 -wd4146)
  target_compile_definitions(main_lib INTERFACE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)

  target_sources(main_lib INTERFACE ${CMAKE_CURRENT_LIST_DIR}/os/nvim.manifest)
else()
  target_compile_options(main_lib INTERFACE -Wall -Wextra -pedantic -Wno-unused-parameter
    -Wstrict-prototypes -std=gnu99 -Wshadow -Wconversion -Wvla
    -Wdouble-promotion
    -Wmissing-noreturn
    -Wmissing-format-attribute
    -Wmissing-prototypes)

  if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(main_lib INTERFACE -fno-common
      $<$<CONFIG:Release>:-Wno-unused-result>
      $<$<CONFIG:RelWithDebInfo>:-Wno-unused-result>
      $<$<CONFIG:MinSizeRel>:-Wno-unused-result>)
  endif()
endif()

# On FreeBSD 64 math.h uses unguarded C11 extension, which taints clang
# 3.4.1 used there.
if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD" AND CMAKE_C_COMPILER_ID MATCHES "Clang")
  target_compile_options(main_lib INTERFACE -Wno-c11-extensions)
endif()

check_c_compiler_flag(-Wimplicit-fallthrough HAVE_WIMPLICIT_FALLTHROUGH_FLAG)
if(HAVE_WIMPLICIT_FALLTHROUGH_FLAG)
  target_compile_options(main_lib INTERFACE -Wimplicit-fallthrough)
endif()

option(ENABLE_COMPILER_SUGGESTIONS "Enable -Wsuggest compiler warnings" OFF)
if(ENABLE_COMPILER_SUGGESTIONS)
  # Clang doesn't have -Wsuggest-attribute so check for each one.
  check_c_compiler_flag(-Wsuggest-attribute=pure HAVE_WSUGGEST_ATTRIBUTE_PURE)
  if(HAVE_WSUGGEST_ATTRIBUTE_PURE)
    target_compile_options(main_lib INTERFACE -Wsuggest-attribute=pure)
  endif()

  check_c_compiler_flag(-Wsuggest-attribute=const HAVE_WSUGGEST_ATTRIBUTE_CONST)
  if(HAVE_WSUGGEST_ATTRIBUTE_CONST)
    target_compile_options(main_lib INTERFACE -Wsuggest-attribute=const)
  endif()

  check_c_compiler_flag(-Wsuggest-attribute=malloc HAVE_WSUGGEST_ATTRIBUTE_MALLOC)
  if(HAVE_WSUGGEST_ATTRIBUTE_MALLOC)
    target_compile_options(main_lib INTERFACE -Wsuggest-attribute=malloc)
  endif()

  check_c_compiler_flag(-Wsuggest-attribute=cold HAVE_WSUGGEST_ATTRIBUTE_COLD)
  if(HAVE_WSUGGEST_ATTRIBUTE_COLD)
    target_compile_options(main_lib INTERFACE -Wsuggest-attribute=cold)
  endif()
endif()

if(MINGW)
  # Use POSIX compatible stdio in Mingw
  target_compile_definitions(main_lib INTERFACE __USE_MINGW_ANSI_STDIO)
endif()
if(WIN32)
  target_compile_definitions(main_lib INTERFACE _WIN32_WINNT=0x0602 MSWIN)
endif()

check_c_compiler_flag(-fdiagnostics-color=auto HAS_DIAG_COLOR_FLAG)
if(HAS_DIAG_COLOR_FLAG)
  if(CMAKE_GENERATOR MATCHES "Ninja")
    target_compile_options(main_lib INTERFACE -fdiagnostics-color=always)
  else()
    target_compile_options(main_lib INTERFACE -fdiagnostics-color=auto)
  endif()
endif()

option(CI_BUILD "CI, extra flags will be set" OFF)
if(CI_BUILD)
  message(STATUS "CI build enabled")
  if(MSVC)
    target_compile_options(main_lib INTERFACE -WX)
  else()
    target_compile_options(main_lib INTERFACE -Werror)
  endif()
endif()

option(UNSIGNED_CHAR "Set char to be unsigned" OFF)
if(UNSIGNED_CHAR)
  target_compile_options(main_lib INTERFACE -funsigned-char)
endif()

target_compile_definitions(main_lib INTERFACE INCLUDE_GENERATED_DECLARATIONS)

# Remove --sort-common from linker flags, as this seems to cause bugs (see #2641, #3374).
# TODO: Figure out the root cause.
if(CMAKE_EXE_LINKER_FLAGS MATCHES "--sort-common" OR
   CMAKE_SHARED_LINKER_FLAGS MATCHES "--sort-common" OR
   CMAKE_MODULE_LINKER_FLAGS MATCHES "--sort-common")
  message(STATUS "Removing --sort-common from linker flags")
  string(REGEX REPLACE ",--sort-common(=[^,]+)?" "" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
  string(REGEX REPLACE ",--sort-common(=[^,]+)?" "" CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS}")
  string(REGEX REPLACE ",--sort-common(=[^,]+)?" "" CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS}")

  # If no linker flags remain for a -Wl argument, remove it.
  # '-Wl$' will match LDFLAGS="-Wl,--sort-common",
  # '-Wl ' will match LDFLAGS="-Wl,--sort-common -Wl,..."
  string(REGEX REPLACE "-Wl($| )" "" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
  string(REGEX REPLACE "-Wl($| )" "" CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS}")
  string(REGEX REPLACE "-Wl($| )" "" CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS}")
endif()

if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
  if(CMAKE_SYSTEM_NAME STREQUAL "SunOS")
    target_link_libraries(nvim PRIVATE -Wl,--no-undefined -lsocket)
  elseif(NOT APPLE)
    target_link_libraries(nvim PRIVATE -Wl,--no-undefined)
  endif()

  # For O_CLOEXEC, O_DIRECTORY, and O_NOFOLLOW flags on older systems
  # (pre POSIX.1-2008: glibc 2.11 and earlier). #4042
  # For ptsname(). #6743
  target_compile_definitions(main_lib INTERFACE _GNU_SOURCE)
endif()

option(ENABLE_GCOV "Enable gcov support" OFF)
if(ENABLE_GCOV)
  if(ENABLE_TSAN)
    # GCOV and TSAN results in false data race reports
    message(FATAL_ERROR "ENABLE_GCOV cannot be used with ENABLE_TSAN")
  endif()
  message(STATUS "Enabling gcov support")
  target_compile_options(main_lib INTERFACE --coverage)
  target_link_libraries(main_lib INTERFACE --coverage)
  target_compile_definitions(main_lib INTERFACE USE_GCOV)
endif()

if(WIN32)
  if(MINGW)
    # Enable wmain
    target_link_libraries(nvim PRIVATE -municode)
  endif()
elseif(APPLE)
  target_link_libraries(nvim PRIVATE "-framework CoreServices")
endif()

if(UNIX)
  # -fstack-protector breaks non Unix builds even in Mingw-w64
  check_c_compiler_flag(-fstack-protector-strong HAS_FSTACK_PROTECTOR_STRONG_FLAG)
  if(HAS_FSTACK_PROTECTOR_STRONG_FLAG)
    target_compile_options(main_lib INTERFACE -fstack-protector-strong)
    target_link_libraries(main_lib INTERFACE -fstack-protector-strong)
  else()
    check_c_compiler_flag(-fstack-protector HAS_FSTACK_PROTECTOR_FLAG)
    if(HAS_FSTACK_PROTECTOR_FLAG)
      target_compile_options(main_lib INTERFACE -fstack-protector --param ssp-buffer-size=4)
      target_link_libraries(main_lib INTERFACE -fstack-protector --param ssp-buffer-size=4)
    endif()
  endif()
endif()

set(GENERATOR_DIR ${CMAKE_CURRENT_LIST_DIR}/generators)
set(GENERATED_DIR ${PROJECT_BINARY_DIR}/src/nvim/auto)
set(BINARY_LIB_DIR ${PROJECT_BINARY_DIR}/lib/nvim/)
set(API_DISPATCH_GENERATOR ${GENERATOR_DIR}/gen_api_dispatch.lua)
set(API_UI_EVENTS_GENERATOR ${GENERATOR_DIR}/gen_api_ui_events.lua)
set(GENERATOR_C_GRAMMAR ${GENERATOR_DIR}/c_grammar.lua)
set(GENERATOR_HASHY ${GENERATOR_DIR}/hashy.lua)
set(API_METADATA ${PROJECT_BINARY_DIR}/api_metadata.mpack)
set(FUNCS_DATA ${PROJECT_BINARY_DIR}/funcs_data.mpack)
set(LUA_API_C_BINDINGS ${GENERATED_DIR}/lua_api_c_bindings.generated.c)
set(HEADER_GENERATOR ${GENERATOR_DIR}/gen_declarations.lua)
set(GENERATED_INCLUDES_DIR ${PROJECT_BINARY_DIR}/include)
set(GENERATED_API_DISPATCH ${GENERATED_DIR}/api/private/dispatch_wrappers.generated.h)
set(GENERATED_FUNCS_METADATA ${GENERATED_DIR}/api/private/funcs_metadata.generated.h)
set(GENERATED_UI_EVENTS_CALL ${GENERATED_DIR}/ui_events_call.generated.h)
set(GENERATED_UI_EVENTS_REMOTE ${GENERATED_DIR}/ui_events_remote.generated.h)
set(GENERATED_UI_EVENTS_CLIENT ${GENERATED_DIR}/ui_events_client.generated.h)
set(GENERATED_UI_EVENTS_METADATA ${GENERATED_DIR}/api/private/ui_events_metadata.generated.h)
set(GENERATED_EX_CMDS_ENUM ${GENERATED_INCLUDES_DIR}/ex_cmds_enum.generated.h)
set(GENERATED_EX_CMDS_DEFS ${GENERATED_DIR}/ex_cmds_defs.generated.h)
set(GENERATED_FUNCS ${GENERATED_DIR}/funcs.generated.h)
set(GENERATED_KEYSETS ${GENERATED_DIR}/keysets.generated.h)
set(GENERATED_KEYSETS_DEFS ${GENERATED_DIR}/keysets_defs.generated.h)
set(GENERATED_EVENTS_ENUM ${GENERATED_INCLUDES_DIR}/auevents_enum.generated.h)
set(GENERATED_EVENTS_NAMES_MAP ${GENERATED_DIR}/auevents_name_map.generated.h)
set(GENERATED_OPTIONS ${GENERATED_DIR}/options.generated.h)
set(EX_CMDS_GENERATOR ${GENERATOR_DIR}/gen_ex_cmds.lua)
set(FUNCS_GENERATOR ${GENERATOR_DIR}/gen_eval.lua)
set(EVENTS_GENERATOR ${GENERATOR_DIR}/gen_events.lua)
set(KEYSETS_GENERATOR ${GENERATOR_DIR}/gen_keysets.lua)
set(OPTIONS_GENERATOR ${GENERATOR_DIR}/gen_options.lua)
set(UNICODE_TABLES_GENERATOR ${GENERATOR_DIR}/gen_unicode_tables.lua)
set(UNICODE_DIR ${PROJECT_SOURCE_DIR}/src/unicode)
set(GENERATED_UNICODE_TABLES ${GENERATED_DIR}/unicode_tables.generated.h)
set(VIM_MODULE_FILE ${GENERATED_DIR}/lua/vim_module.generated.h)
set(LUA_EDITOR_MODULE_SOURCE ${PROJECT_SOURCE_DIR}/runtime/lua/vim/_editor.lua)
set(LUA_SHARED_MODULE_SOURCE ${PROJECT_SOURCE_DIR}/runtime/lua/vim/shared.lua)
set(LUA_LOADER_MODULE_SOURCE ${PROJECT_SOURCE_DIR}/runtime/lua/vim/loader.lua)
set(LUA_INSPECT_MODULE_SOURCE ${PROJECT_SOURCE_DIR}/runtime/lua/vim/inspect.lua)
set(LUA_FS_MODULE_SOURCE ${PROJECT_SOURCE_DIR}/runtime/lua/vim/fs.lua)
set(LUA_F_MODULE_SOURCE ${PROJECT_SOURCE_DIR}/runtime/lua/vim/F.lua)
set(LUA_META_MODULE_SOURCE ${PROJECT_SOURCE_DIR}/runtime/lua/vim/_meta.lua)
set(LUA_FILETYPE_MODULE_SOURCE ${PROJECT_SOURCE_DIR}/runtime/lua/vim/filetype.lua)
set(LUA_INIT_PACKAGES_MODULE_SOURCE ${PROJECT_SOURCE_DIR}/runtime/lua/vim/_init_packages.lua)
set(LUA_KEYMAP_MODULE_SOURCE ${PROJECT_SOURCE_DIR}/runtime/lua/vim/keymap.lua)
set(CHAR_BLOB_GENERATOR ${GENERATOR_DIR}/gen_char_blob.lua)
set(LUAJIT_RUNTIME_DIR ${DEPS_PREFIX}/share/luajit-2.1.0-beta3/jit)

glob_wrapper(UNICODE_FILES ${UNICODE_DIR}/*.txt)
glob_wrapper(API_HEADERS api/*.h)
list(REMOVE_ITEM API_HEADERS ${CMAKE_CURRENT_LIST_DIR}/api/ui_events.in.h)
glob_wrapper(MSGPACK_RPC_HEADERS msgpack_rpc/*.h)

target_include_directories(main_lib INTERFACE ${GENERATED_DIR})
target_include_directories(main_lib INTERFACE ${CACHED_GENERATED_DIR})
target_include_directories(main_lib INTERFACE ${GENERATED_INCLUDES_DIR})
target_include_directories(main_lib INTERFACE "${PROJECT_BINARY_DIR}/cmake.config")
target_include_directories(main_lib INTERFACE "${PROJECT_SOURCE_DIR}/src")

file(MAKE_DIRECTORY ${TOUCHES_DIR})
file(MAKE_DIRECTORY ${GENERATED_DIR})
file(MAKE_DIRECTORY ${GENERATED_INCLUDES_DIR})

glob_wrapper(NVIM_SOURCES *.c)
glob_wrapper(NVIM_HEADERS *.h)
glob_wrapper(EXTERNAL_SOURCES ../xdiff/*.c ../mpack/*.c ../cjson/*.c ../klib/*.c)
glob_wrapper(EXTERNAL_HEADERS ../xdiff/*.h ../mpack/*.h ../cjson/*.h ../klib/*.h)

if(PREFER_LUA)
  # luajit not used, use a vendored copy of the bit module
  list(APPEND EXTERNAL_SOURCES ${PROJECT_SOURCE_DIR}/src/bit.c)
  target_compile_definitions(main_lib INTERFACE NVIM_VENDOR_BIT)
endif()

foreach(subdir
        os
        api
        api/private
        msgpack_rpc
        tui
        event
        eval
        lua
        viml
        viml/parser
       )

  file(MAKE_DIRECTORY ${GENERATED_DIR}/${subdir})
  file(MAKE_DIRECTORY ${GENERATED_INCLUDES_DIR}/${subdir})
  glob_wrapper(sources ${subdir}/*.c)
  glob_wrapper(headers ${subdir}/*.h)
  list(APPEND NVIM_SOURCES ${sources})
  list(APPEND NVIM_HEADERS ${headers})
endforeach()

# Sort file lists to ensure generated files are created in the same order from
# build to build.
list(SORT NVIM_SOURCES)
list(SORT NVIM_HEADERS)

list(APPEND LINT_NVIM_SOURCES ${NVIM_SOURCES} ${NVIM_HEADERS})

foreach(sfile ${NVIM_SOURCES})
  get_filename_component(f ${sfile} NAME)
  if(${f} MATCHES "^(regexp_nfa.c)$")
    list(APPEND to_remove ${sfile})
  endif()
  if(${f} MATCHES "^(regexp_bt.c)$")
    list(APPEND to_remove ${sfile})
  endif()
  if(WIN32 AND ${f} MATCHES "^(pty_process_unix.c)$")
    list(APPEND to_remove ${sfile})
  endif()
  if(NOT WIN32 AND ${f} MATCHES "^(pty_process_win.c)$")
    list(APPEND to_remove ${sfile})
  endif()
  if(NOT WIN32 AND ${f} MATCHES "^(pty_conpty_win.c)$")
    list(APPEND to_remove ${sfile})
  endif()
  if(NOT WIN32 AND ${f} MATCHES "^(os_win_console.c)$")
    list(APPEND to_remove ${sfile})
  endif()
endforeach()

list(REMOVE_ITEM NVIM_SOURCES ${to_remove})

# xdiff, mpack, lua-cjson: inlined external project, we don't maintain it. #9306
if(MSVC)
  set_source_files_properties(
    ${EXTERNAL_SOURCES} PROPERTIES COMPILE_FLAGS "${COMPILE_FLAGS} -wd4090 -wd4244 -wd4267")
else()
  set_source_files_properties(
    ${EXTERNAL_SOURCES} PROPERTIES COMPILE_FLAGS "${COMPILE_FLAGS} -Wno-conversion -Wno-missing-noreturn -Wno-missing-format-attribute -Wno-double-promotion -Wno-strict-prototypes -Wno-misleading-indentation")
endif()

# Log level (NVIM_LOG_DEBUG in log.h)
if($ENV{CI})
  # Don't debug log on CI, it gets too verbose in the main build log.
  # TODO(bfredl): debug log level also exposes some errors with EXITFREE in ASAN build.
  set(LOG_DEBUG FALSE)
elseif(LOG_DEBUG)
  target_compile_definitions(nvim PRIVATE NVIM_LOG_DEBUG)
else()
  # Minimize logging for release-type builds.
  target_compile_definitions(nvim PRIVATE $<$<CONFIG:Debug>:NVIM_LOG_DEBUG>)
endif()

if(ENABLE_ASAN_UBSAN OR ENABLE_MSAN OR ENABLE_TSAN)
  target_compile_definitions(main_lib INTERFACE EXITFREE)
endif()

get_target_property(prop main_lib INTERFACE_COMPILE_DEFINITIONS)
foreach(gen_cdef DO_NOT_DEFINE_EMPTY_ATTRIBUTES ${prop})
  if(NOT ${gen_cdef} MATCHES "INCLUDE_GENERATED_DECLARATIONS")
    list(APPEND gen_cflags "-D${gen_cdef}")
  endif()
endforeach()

get_directory_property(targets BUILDSYSTEM_TARGETS)
foreach(target ${targets})
  get_target_property(prop ${target} INTERFACE_INCLUDE_DIRECTORIES)
  foreach(gen_include ${prop})
    list(APPEND gen_cflags "-I${gen_include}")
  endforeach()
endforeach()

if(APPLE AND CMAKE_OSX_SYSROOT)
  list(APPEND gen_cflags "-isysroot")
  list(APPEND gen_cflags "${CMAKE_OSX_SYSROOT}")
endif()
set(gen_cflags ${gen_cflags} -O2)

set(NVIM_VERSION_GIT_H ${PROJECT_BINARY_DIR}/cmake.config/auto/versiondef_git.h)
add_custom_target(update_version_stamp
  COMMAND ${CMAKE_COMMAND}
    -D NVIM_VERSION_MAJOR=${NVIM_VERSION_MAJOR}
    -D NVIM_VERSION_MINOR=${NVIM_VERSION_MINOR}
    -D NVIM_VERSION_PATCH=${NVIM_VERSION_PATCH}
    -D NVIM_VERSION_PRERELEASE=${NVIM_VERSION_PRERELEASE}
    -D OUTPUT=${NVIM_VERSION_GIT_H}
    -D NVIM_SOURCE_DIR=${CMAKE_SOURCE_DIR}
    -P ${PROJECT_SOURCE_DIR}/cmake/GenerateVersion.cmake
  BYPRODUCTS ${NVIM_VERSION_GIT_H})

set(NVIM_VERSION_DEF_H ${PROJECT_BINARY_DIR}/cmake.config/auto/versiondef.h)
add_custom_command(
  OUTPUT "${NVIM_VERSION_DEF_H}"
  COMMAND "${CMAKE_COMMAND}"
    -E copy
    "${PROJECT_BINARY_DIR}/cmake.config/auto/versiondef-$<CONFIG>.h"
    "${NVIM_VERSION_DEF_H}"
  DEPENDS "${PROJECT_BINARY_DIR}/cmake.config/auto/versiondef-$<CONFIG>.h")

# NVIM_GENERATED_FOR_HEADERS: generated headers to be included in headers
# NVIM_GENERATED_FOR_SOURCES: generated headers to be included in sources
# NVIM_GENERATED_SOURCES: generated source files
# These lists must be mutually exclusive.
foreach(sfile ${NVIM_SOURCES}
              "${CMAKE_CURRENT_LIST_DIR}/regexp_bt.c"
              "${CMAKE_CURRENT_LIST_DIR}/regexp_nfa.c"
              ${GENERATED_API_DISPATCH}
              "${GENERATED_UI_EVENTS_CALL}"
              "${GENERATED_UI_EVENTS_REMOTE}"
              "${GENERATED_KEYSETS}"
              "${GENERATED_UI_EVENTS_CLIENT}"
              )
  get_filename_component(full_d ${sfile} DIRECTORY)
  file(RELATIVE_PATH d "${CMAKE_CURRENT_LIST_DIR}" "${full_d}")
  if(${d} MATCHES "^[.][.]|auto/")
    file(RELATIVE_PATH d "${GENERATED_DIR}" "${full_d}")
  endif()
  get_filename_component(f ${sfile} NAME)
  get_filename_component(r ${sfile} NAME_WE)
  if(NOT ${d} EQUAL ".")
    set(f "${d}/${f}")
    set(r "${d}/${r}")
  endif()
  set(gf_c_h "${GENERATED_DIR}/${r}.c.generated.h")
  set(gf_h_h "${GENERATED_INCLUDES_DIR}/${r}.h.generated.h")
  set(gf_i "${GENERATED_DIR}/${r}.i")

  if(MSVC)
    set(PREPROC_OUTPUT /P /Fi${gf_i} /nologo)
  else()
    set(PREPROC_OUTPUT -E -o ${gf_i})
  endif()

  set(depends "${HEADER_GENERATOR}" "${sfile}")
  if("${f}" STREQUAL "version.c")
    # Ensure auto/versiondef_git.h exists after "make clean".
    list(APPEND depends update_version_stamp "${NVIM_VERSION_GIT_H}" "${NVIM_VERSION_DEF_H}")
  endif()
  add_custom_command(
    OUTPUT "${gf_c_h}" "${gf_h_h}"
    COMMAND ${CMAKE_C_COMPILER} ${sfile} ${PREPROC_OUTPUT} ${gen_cflags}
    COMMAND "${LUA_PRG}" "${HEADER_GENERATOR}" "${sfile}" "${gf_c_h}" "${gf_h_h}" "${gf_i}"
    DEPENDS ${depends})
  list(APPEND NVIM_GENERATED_FOR_SOURCES "${gf_c_h}")
  list(APPEND NVIM_GENERATED_FOR_HEADERS "${gf_h_h}")
  if(${d} MATCHES "^api$" AND NOT ${f} MATCHES "^api/helpers.c$")
    list(APPEND API_HEADERS ${gf_h_h})
  endif()
endforeach()

add_custom_command(OUTPUT ${GENERATED_UNICODE_TABLES}
  COMMAND ${LUA_PRG} ${UNICODE_TABLES_GENERATOR}
                     ${UNICODE_DIR}
                     ${GENERATED_UNICODE_TABLES}
  DEPENDS
    ${UNICODE_TABLES_GENERATOR}
    ${UNICODE_FILES}
)

add_custom_command(
  OUTPUT ${GENERATED_API_DISPATCH} ${GENERATED_FUNCS_METADATA}
         ${API_METADATA} ${LUA_API_C_BINDINGS}
  COMMAND ${LUA_GEN_PRG} ${API_DISPATCH_GENERATOR} ${CMAKE_CURRENT_LIST_DIR}
                         ${GENERATED_API_DISPATCH}
                         ${GENERATED_FUNCS_METADATA} ${API_METADATA}
                         ${LUA_API_C_BINDINGS}
                         ${API_HEADERS}
  DEPENDS
    ${API_HEADERS}
    ${MSGPACK_RPC_HEADERS}
    ${API_DISPATCH_GENERATOR}
    ${GENERATOR_C_GRAMMAR}
    ${CMAKE_CURRENT_LIST_DIR}/api/dispatch_deprecated.lua
)

add_custom_command(
  OUTPUT ${VIM_MODULE_FILE}
  COMMAND ${CMAKE_COMMAND} -E env
      "LUAC_PRG=${LUAC_PRG}"
      ${LUA_PRG} ${CHAR_BLOB_GENERATOR} -c ${VIM_MODULE_FILE}
      ${LUA_INIT_PACKAGES_MODULE_SOURCE} "vim._init_packages"
      ${LUA_INSPECT_MODULE_SOURCE} "vim.inspect"
      ${LUA_FS_MODULE_SOURCE} "vim.fs"
      ${LUA_EDITOR_MODULE_SOURCE} "vim._editor"
      ${LUA_SHARED_MODULE_SOURCE} "vim.shared"
      ${LUA_LOADER_MODULE_SOURCE} "vim.loader"
      ${LUA_F_MODULE_SOURCE} "vim.F"
      ${LUA_META_MODULE_SOURCE} "vim._meta"
      ${LUA_FILETYPE_MODULE_SOURCE} "vim.filetype"
      ${LUA_KEYMAP_MODULE_SOURCE} "vim.keymap"
  DEPENDS
    ${CHAR_BLOB_GENERATOR}
    ${LUA_INIT_PACKAGES_MODULE_SOURCE}
    ${LUA_EDITOR_MODULE_SOURCE}
    ${LUA_SHARED_MODULE_SOURCE}
    ${LUA_LOADER_MODULE_SOURCE}
    ${LUA_INSPECT_MODULE_SOURCE}
    ${LUA_F_MODULE_SOURCE}
    ${LUA_META_MODULE_SOURCE}
    ${LUA_FILETYPE_MODULE_SOURCE}
    ${LUA_LOAD_PACKAGE_MODULE_SOURCE}
    ${LUA_KEYMAP_MODULE_SOURCE}
  VERBATIM
)

list(APPEND NVIM_GENERATED_SOURCES
  "${LUA_API_C_BINDINGS}"
)

add_custom_command(
  OUTPUT ${GENERATED_UI_EVENTS_CALL}
         ${GENERATED_UI_EVENTS_REMOTE}
         ${GENERATED_UI_EVENTS_METADATA}
         ${GENERATED_UI_EVENTS_CLIENT}
  COMMAND ${LUA_GEN_PRG} ${API_UI_EVENTS_GENERATOR} ${CMAKE_CURRENT_LIST_DIR}
                         ${CMAKE_CURRENT_LIST_DIR}/api/ui_events.in.h
                         ${GENERATED_UI_EVENTS_CALL}
                         ${GENERATED_UI_EVENTS_REMOTE}
                         ${GENERATED_UI_EVENTS_METADATA}
                         ${GENERATED_UI_EVENTS_CLIENT}
  DEPENDS
    ${API_UI_EVENTS_GENERATOR}
    ${GENERATOR_C_GRAMMAR}
    ${CMAKE_CURRENT_LIST_DIR}/api/ui_events.in.h
)

list(APPEND NVIM_GENERATED_FOR_HEADERS
  "${GENERATED_EX_CMDS_ENUM}"
  "${GENERATED_EVENTS_ENUM}"
  "${GENERATED_KEYSETS_DEFS}"
)

list(APPEND NVIM_GENERATED_FOR_SOURCES
  "${GENERATED_API_DISPATCH}"
  "${GENERATED_EX_CMDS_DEFS}"
  "${GENERATED_EVENTS_NAMES_MAP}"
  "${GENERATED_KEYSETS}"
  "${GENERATED_OPTIONS}"
  "${GENERATED_UNICODE_TABLES}"
  "${VIM_MODULE_FILE}"
)

list(APPEND NVIM_GENERATED_SOURCES
  "${PROJECT_BINARY_DIR}/cmake.config/auto/pathdef.c"
)

add_custom_command(OUTPUT ${GENERATED_EX_CMDS_ENUM} ${GENERATED_EX_CMDS_DEFS}
  COMMAND ${LUA_PRG} ${EX_CMDS_GENERATOR}
      ${CMAKE_CURRENT_LIST_DIR} ${GENERATED_INCLUDES_DIR} ${GENERATED_DIR}
  DEPENDS ${EX_CMDS_GENERATOR} ${CMAKE_CURRENT_LIST_DIR}/ex_cmds.lua
)

add_custom_command(OUTPUT ${GENERATED_FUNCS} ${FUNCS_DATA}
  COMMAND ${LUA_PRG} ${FUNCS_GENERATOR}
      ${CMAKE_CURRENT_LIST_DIR} ${LUA_SHARED_MODULE_SOURCE} ${GENERATED_DIR} ${API_METADATA} ${FUNCS_DATA}
  DEPENDS ${FUNCS_GENERATOR} ${CMAKE_CURRENT_LIST_DIR}/eval.lua ${API_METADATA}
)
list(APPEND NVIM_GENERATED_FOR_SOURCES
  "${GENERATED_FUNCS}")

add_custom_command(OUTPUT ${GENERATED_EVENTS_ENUM} ${GENERATED_EVENTS_NAMES_MAP}
  COMMAND ${LUA_PRG} ${EVENTS_GENERATOR}
      ${CMAKE_CURRENT_LIST_DIR} ${GENERATED_EVENTS_ENUM} ${GENERATED_EVENTS_NAMES_MAP}
  DEPENDS ${EVENTS_GENERATOR} ${CMAKE_CURRENT_LIST_DIR}/auevents.lua
)

add_custom_command(OUTPUT ${GENERATED_KEYSETS} ${GENERATED_KEYSETS_DEFS}
  COMMAND ${LUA_PRG} ${KEYSETS_GENERATOR}
      ${CMAKE_CURRENT_LIST_DIR} ${LUA_SHARED_MODULE_SOURCE} ${GENERATED_KEYSETS} ${GENERATED_KEYSETS_DEFS}
  DEPENDS ${KEYSETS_GENERATOR} ${CMAKE_CURRENT_LIST_DIR}/api/keysets.lua ${GENERATOR_HASHY}
)

add_custom_command(OUTPUT ${GENERATED_OPTIONS}
  COMMAND ${LUA_PRG} ${OPTIONS_GENERATOR}
                     ${CMAKE_CURRENT_LIST_DIR} ${GENERATED_OPTIONS}
  DEPENDS ${OPTIONS_GENERATOR} ${CMAKE_CURRENT_LIST_DIR}/options.lua
)

# NVIM_GENERATED_FOR_SOURCES and NVIM_GENERATED_FOR_HEADERS must be mutually exclusive.
foreach(hfile ${NVIM_GENERATED_FOR_HEADERS})
  list(FIND NVIM_GENERATED_FOR_SOURCES ${hfile} hfile_idx)
  if(NOT ${hfile_idx} EQUAL -1)
    message(FATAL_ERROR "File included in both NVIM_GENERATED_FOR_HEADERS and NVIM_GENERATED_FOR_SOURCES")
  endif()
endforeach()

if (CMAKE_SYSTEM_NAME MATCHES "OpenBSD")
  target_link_libraries(main_lib INTERFACE pthread c++abi)
endif()

if(WIN32)
  target_link_libraries(main_lib INTERFACE netapi32)
endif()

if(UNIX)
  target_link_libraries(main_lib INTERFACE m)
  if (NOT CMAKE_SYSTEM_NAME STREQUAL "SunOS")
    target_link_libraries(main_lib INTERFACE util)
  endif()
endif()

if(PREFER_LUA)
  message(STATUS "luajit not used, skipping unit tests")
else()
  glob_wrapper(UNIT_TEST_FIXTURES ${PROJECT_SOURCE_DIR}/test/unit/fixtures/*.c)
  target_sources(nvim PRIVATE ${UNIT_TEST_FIXTURES})
  target_compile_definitions(nvim PRIVATE UNIT_TESTING)
endif()

target_sources(main_lib INTERFACE
  ${NVIM_GENERATED_FOR_SOURCES}
  ${NVIM_GENERATED_FOR_HEADERS}
  ${NVIM_GENERATED_SOURCES}
  ${NVIM_SOURCES}
  ${NVIM_HEADERS}
  ${EXTERNAL_SOURCES}
  ${EXTERNAL_HEADERS})

set_target_properties(nvim
  PROPERTIES
  EXPORT_COMPILE_COMMANDS ON
  ENABLE_EXPORTS TRUE)

if(${CMAKE_VERSION} VERSION_LESS 3.20)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

target_link_libraries(nvim PRIVATE main_lib PUBLIC libuv)
install_helper(TARGETS nvim)
if(MSVC)
  install(FILES $<TARGET_PDB_FILE:nvim> DESTINATION ${CMAKE_INSTALL_BINDIR} OPTIONAL)
endif()

if(ENABLE_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT IPO_SUPPORTED)
  if(IPO_SUPPORTED)
    set_target_properties(nvim PROPERTIES
      INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
      INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE
      INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL TRUE)
  endif()
endif()

if(WIN32)
  # Copy DLLs and third-party tools to bin/ and install them along with nvim
  add_custom_target(nvim_runtime_deps ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_BINARY_DIR}/windows_runtime_deps/
      ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
  install(DIRECTORY ${PROJECT_BINARY_DIR}/windows_runtime_deps/
    DESTINATION ${CMAKE_INSTALL_BINDIR})

  add_custom_target(nvim_dll_deps DEPENDS nvim
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PROJECT_BINARY_DIR}/windows_runtime_deps
    COMMAND ${CMAKE_COMMAND}
      -D CMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}
      -D BINARY="${PROJECT_BINARY_DIR}/bin/nvim${CMAKE_EXECUTABLE_SUFFIX}"
      -D DST=${PROJECT_BINARY_DIR}/windows_runtime_deps
      -P ${PROJECT_SOURCE_DIR}/cmake/WindowsDllCopy.cmake)
  add_dependencies(nvim_runtime_deps nvim_dll_deps)

  if(USE_BUNDLED_NVIMQT)
    set(NVIMQT_DEPS
      # Dependencies for neovim-qt
      bearer/qgenericbearer.dll
      iconengines/qsvgicon.dll
      imageformats/qgif.dll
      imageformats/qicns.dll
      imageformats/qico.dll
      imageformats/qjpeg.dll
      imageformats/qsvg.dll
      imageformats/qtga.dll
      imageformats/qtiff.dll
      imageformats/qwbmp.dll
      imageformats/qwebp.dll
      platforms/qwindows.dll
      styles/qwindowsvistastyle.dll
      translations/qt_ar.qm
      translations/qt_bg.qm
      translations/qt_ca.qm
      translations/qt_cs.qm
      translations/qt_da.qm
      translations/qt_de.qm
      translations/qt_en.qm
      translations/qt_es.qm
      translations/qt_fi.qm
      translations/qt_fr.qm
      translations/qt_gd.qm
      translations/qt_he.qm
      translations/qt_hu.qm
      translations/qt_it.qm
      translations/qt_ja.qm
      translations/qt_ko.qm
      translations/qt_lv.qm
      translations/qt_pl.qm
      translations/qt_ru.qm
      translations/qt_sk.qm
      translations/qt_uk.qm
      D3Dcompiler_47.dll
      libEGL.dll
      libgcc_s_seh-1.dll
      libGLESv2.dll
      libstdc++-6.dll
      libwinpthread-1.dll
      nvim-qt.exe
      opengl32sw.dll
      Qt5Core.dll
      Qt5Gui.dll
      Qt5Network.dll
      Qt5Svg.dll
      Qt5Widgets.dll
      )
  endif()

  # A CMake script is used for copying the files to avoid the
  # "command line is too long" error that occurs when Ninja tries running
  # a command that exceeds the length limit (8191 characters) on Windows.
  # See https://developercommunity.visualstudio.com/content/problem/212207/file-open-cmake-the-command-line-is-too-long.html
  set(EXTERNAL_BLOBS_SCRIPT
    "file(MAKE_DIRECTORY \"${PROJECT_BINARY_DIR}/windows_runtime_deps/platforms\")")
  foreach(DEP_FILE IN ITEMS
      cat.exe
      diff.exe
      tee.exe
      win32yank.exe
      xxd.exe

      ${NVIMQT_DEPS})
  get_filename_component(DEP_FILE_DIR ${DEP_FILE} DIRECTORY)
  set(EXTERNAL_BLOBS_SCRIPT "${EXTERNAL_BLOBS_SCRIPT}\n"
    "file(COPY \"${DEPS_PREFIX}/bin/${DEP_FILE}\"
    DESTINATION \"${PROJECT_BINARY_DIR}/windows_runtime_deps/${DEP_FILE_DIR}\")")
  endforeach()
  file(WRITE ${PROJECT_BINARY_DIR}/external_blobs.cmake ${EXTERNAL_BLOBS_SCRIPT})
  add_custom_target(external_blobs
    COMMAND ${CMAKE_COMMAND} -P ${PROJECT_BINARY_DIR}/external_blobs.cmake)
  add_dependencies(nvim_runtime_deps external_blobs)
else()
  add_custom_target(nvim_runtime_deps)  # Stub target to avoid CMP0046.
endif()

file(MAKE_DIRECTORY ${BINARY_LIB_DIR})

# install treesitter parser if bundled
if(EXISTS ${DEPS_PREFIX}/lib/nvim/parser)
  glob_wrapper(TREESITTER_PARSERS ${DEPS_PREFIX}/lib/nvim/parser/*)
  foreach(parser_lib ${TREESITTER_PARSERS})
    file(COPY ${parser_lib} DESTINATION ${BINARY_LIB_DIR}/parser)
  endforeach()
endif()

install(DIRECTORY ${BINARY_LIB_DIR}
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/nvim/
  USE_SOURCE_PERMISSIONS)

if(NOT PREFER_LUA)
  # install luajit runtime files if bundled
  if(EXISTS ${LUAJIT_RUNTIME_DIR})
    install(DIRECTORY ${LUAJIT_RUNTIME_DIR}
      DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/nvim/runtime/lua
      USE_SOURCE_PERMISSIONS)
  endif()
endif()

add_library(libnvim STATIC EXCLUDE_FROM_ALL)
if(MSVC)
  set(LIBNVIM_NAME libnvim)
else()
  set(LIBNVIM_NAME nvim)
endif()
set_target_properties(
  libnvim
  PROPERTIES
    OUTPUT_NAME ${LIBNVIM_NAME}
)
target_compile_definitions(libnvim PRIVATE MAKE_LIB)
target_link_libraries(libnvim PRIVATE main_lib PUBLIC libuv)

if(ENABLE_ASAN_UBSAN)
  message(STATUS "Enabling address sanitizer and undefined behavior sanitizer for nvim.")
  if(CI_BUILD)
    # Try to recover from all sanitize issues so we get reports about all failures
    target_compile_options(nvim PRIVATE -fsanitize-recover=all)
  else()
    target_compile_options(nvim PRIVATE -fno-sanitize-recover=all)
  endif()
  target_compile_options(nvim PRIVATE
    -fno-omit-frame-pointer
    -fno-optimize-sibling-calls
    -fsanitize=address
    -fsanitize=undefined)
  target_link_libraries(nvim PRIVATE -fsanitize=address -fsanitize=undefined)
elseif(ENABLE_MSAN)
  message(STATUS "Enabling memory sanitizer for nvim.")
  target_compile_options(nvim PRIVATE
    -fsanitize=memory
    -fsanitize-memory-track-origins
    -fno-omit-frame-pointer
    -fno-optimize-sibling-calls)
  target_link_libraries(nvim PRIVATE -fsanitize=memory -fsanitize-memory-track-origins)
elseif(ENABLE_TSAN)
  message(STATUS "Enabling thread sanitizer for nvim.")
  target_compile_options(nvim PRIVATE -fsanitize=thread -fPIE)
  target_link_libraries(nvim PRIVATE -fsanitize=thread)
endif()

find_program(CLANG_TIDY_PRG clang-tidy)
set(EXCLUDE_CLANG_TIDY typval_encode.c.h ui_events.in.h)
if(WIN32)
  list(APPEND EXCLUDE_CLANG_TIDY
    os/pty_process_unix.h
    os/unix_defs.h)
else()
  list(APPEND EXCLUDE_CLANG_TIDY
    os/win_defs.h
    os/pty_process_win.h
    os/pty_conpty_win.h
    os/os_win_console.h)
endif()
add_glob_target(
  TARGET clang-tidy
  COMMAND ${CLANG_TIDY_PRG}
  FILES ${NVIM_SOURCES} ${NVIM_HEADERS}
  FLAGS --quiet
  EXCLUDE ${EXCLUDE_CLANG_TIDY})
add_custom_target(copy_compile_commands
  COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_BINARY_DIR}/compile_commands.json ${PROJECT_SOURCE_DIR}/compile_commands.json)
add_dependencies(copy_compile_commands nvim)
add_dependencies(clang-tidy copy_compile_commands)

if(CI_BUILD)
  set(LINT_OUTPUT_FORMAT gh_action)
else()
  set(LINT_OUTPUT_FORMAT vs7)
endif()
add_glob_target(
  TARGET lintc-clint
  COMMAND ${PROJECT_SOURCE_DIR}/src/clint.py
  FLAGS --output=${LINT_OUTPUT_FORMAT}
  FILES ${LINT_NVIM_SOURCES}
  EXCLUDE
    tui/terminfo_defs.h)

add_custom_target(uncrustify-version
  COMMAND ${CMAKE_COMMAND}
    -D UNCRUSTIFY_PRG=${UNCRUSTIFY_PRG}
    -D CONFIG_FILE=${PROJECT_SOURCE_DIR}/src/uncrustify.cfg
    -P ${PROJECT_SOURCE_DIR}/cmake/CheckUncrustifyVersion.cmake)

add_glob_target(
  TARGET lintc-uncrustify
  COMMAND ${UNCRUSTIFY_PRG}
  FLAGS -c "${PROJECT_SOURCE_DIR}/src/uncrustify.cfg" -q --check
  FILES ${LINT_NVIM_SOURCES})
add_dependencies(lintc-uncrustify uncrustify-version)

add_custom_target(lintc)
add_dependencies(lintc lintc-clint lintc-uncrustify)

add_custom_target(formatc
  COMMAND ${CMAKE_COMMAND}
    -D FORMAT_PRG=${UNCRUSTIFY_PRG}
    -D LANG=c
    -P ${PROJECT_SOURCE_DIR}/cmake/Format.cmake
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
add_dependencies(formatc uncrustify-version)

add_custom_target(generated-sources DEPENDS
  ${NVIM_GENERATED_FOR_SOURCES}
  ${NVIM_GENERATED_FOR_HEADERS}
  ${NVIM_GENERATED_SOURCES}
)

add_subdirectory(po)
