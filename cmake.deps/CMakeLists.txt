# This is not meant to be included by the top-level.
cmake_minimum_required (VERSION 3.10)
project(NVIM_DEPS C)

if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

# Point CMake at any custom modules we may ship
list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake" "${PROJECT_SOURCE_DIR}/../cmake")

include(CheckCCompilerFlag)
include(Util)

set(DEPS_CMAKE_ARGS
  -D CMAKE_C_COMPILER=${CMAKE_C_COMPILER}
  -D CMAKE_C_STANDARD=99
  -D CMAKE_GENERATOR=${CMAKE_GENERATOR}
  -D CMAKE_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}
  -D CMAKE_POSITION_INDEPENDENT_CODE=ON)

set(DEPS_CMAKE_CACHE_ARGS -DCMAKE_OSX_ARCHITECTURES:STRING=${CMAKE_OSX_ARCHITECTURES})

set_default_buildtype()

get_property(isMultiConfig GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT isMultiConfig)
  list(APPEND DEPS_CMAKE_ARGS -D CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE})
endif()

set(DEFAULT_MAKE_CFLAGS CFLAGS+=-g)

check_c_compiler_flag(-Og HAS_OG_FLAG)
if(HAS_OG_FLAG)
  set(DEFAULT_MAKE_CFLAGS CFLAGS+=-Og ${DEFAULT_MAKE_CFLAGS})
endif()

set(DEPS_INSTALL_DIR "${CMAKE_BINARY_DIR}/usr")
set(DEPS_BIN_DIR "${DEPS_INSTALL_DIR}/bin")
set(DEPS_LIB_DIR "${DEPS_INSTALL_DIR}/lib")
set(DEPS_BUILD_DIR "${CMAKE_BINARY_DIR}/build")
set(DEPS_DOWNLOAD_DIR "${DEPS_BUILD_DIR}/downloads")

list(APPEND DEPS_CMAKE_ARGS -D CMAKE_INSTALL_PREFIX=${DEPS_INSTALL_DIR})

option(USE_BUNDLED "Use bundled dependencies." ON)

option(USE_BUNDLED_UNIBILIUM "Use the bundled unibilium." ${USE_BUNDLED})
option(USE_BUNDLED_LIBTERMKEY "Use the bundled libtermkey." ${USE_BUNDLED})
option(USE_BUNDLED_LIBVTERM "Use the bundled libvterm." ${USE_BUNDLED})
option(USE_BUNDLED_LIBUV "Use the bundled libuv." ${USE_BUNDLED})
option(USE_BUNDLED_MSGPACK "Use the bundled msgpack." ${USE_BUNDLED})
option(USE_BUNDLED_LUAJIT "Use the bundled version of luajit." ${USE_BUNDLED})
option(USE_BUNDLED_LUAROCKS "Use the bundled version of luarocks." ${USE_BUNDLED})
option(USE_BUNDLED_LUV "Use the bundled version of luv." ${USE_BUNDLED})
#XXX(tarruda): Lua is only used for debugging the functional test client, don't
# build it unless explicitly requested
option(USE_BUNDLED_LUA "Use the bundled version of lua." OFF)
option(USE_BUNDLED_TS_PARSERS "Use the bundled treesitter parsers." ${USE_BUNDLED})
option(USE_BUNDLED_TS "Use the bundled treesitter runtime." ${USE_BUNDLED})

if(USE_BUNDLED AND MSVC)
  option(USE_BUNDLED_GETTEXT "Use the bundled version of gettext." ON)
  option(USE_BUNDLED_LIBICONV "Use the bundled version of libiconv." ON)
else()
  option(USE_BUNDLED_GETTEXT "Use the bundled version of gettext." OFF)
  option(USE_BUNDLED_LIBICONV "Use the bundled version of libiconv." OFF)
endif()

if(WIN32)
  option(USE_BUNDLED_NVIMQT "Bundle neovim-qt" ON)
endif()

option(USE_EXISTING_SRC_DIR "Skip download of deps sources in case of existing source directory." OFF)

find_package(Git)
if(NOT Git_FOUND)
  message(FATAL_ERROR "Git is required to apply patches.")
endif()

if(UNIX)
  find_program(MAKE_PRG NAMES gmake make)
  if(NOT MAKE_PRG)
    message(FATAL_ERROR "GNU Make is required to build the dependencies.")
  else()
    message(STATUS "Found GNU Make at ${MAKE_PRG}")
  endif()
endif()

# When using make, use the $(MAKE) variable to avoid warning about the job
# server.
if(CMAKE_GENERATOR MATCHES "Makefiles")
  set(MAKE_PRG "$(MAKE)")
endif()

if(MINGW AND CMAKE_GENERATOR MATCHES "Ninja")
  find_program(MAKE_PRG NAMES mingw32-make)
  if(NOT MAKE_PRG)
    message(FATAL_ERROR "GNU Make for mingw32 is required to build the dependencies.")
  else()
    message(STATUS "Found GNU Make for mingw32: ${MAKE_PRG}")
  endif()
endif()

set(DEPS_C_COMPILER "${CMAKE_C_COMPILER}")

if(CMAKE_OSX_SYSROOT)
  set(DEPS_C_COMPILER "${DEPS_C_COMPILER} -isysroot${CMAKE_OSX_SYSROOT}")
endif()

if(CMAKE_OSX_ARCHITECTURES)
  # The LuaJIT build does not like being passed multiple `-arch` flags
  # so we handle a universal build the old-fashioned way.
  set(LUAJIT_C_COMPILER "${DEPS_C_COMPILER}")
  foreach(ARCH IN LISTS CMAKE_OSX_ARCHITECTURES)
    set(DEPS_C_COMPILER "${DEPS_C_COMPILER} -arch ${ARCH}")
  endforeach()
endif()

# If the macOS deployment target is not set manually (via $MACOSX_DEPLOYMENT_TARGET),
# fall back to local system version. Needs to be done here and in top-level CMakeLists.txt.
if(APPLE)
  if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    execute_process(COMMAND sw_vers -productVersion
                    OUTPUT_VARIABLE MACOS_VERSION
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "${MACOS_VERSION}")
  endif()
  message(STATUS "Using deployment target ${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()

include(ExternalProject)
set_directory_properties(PROPERTIES EP_PREFIX "${DEPS_BUILD_DIR}")

set(LIBUV_URL https://github.com/libuv/libuv/archive/62c2374a8c005ce9e42088965f8f8af2532c177b.tar.gz)
set(LIBUV_SHA256 c7e89137da65a1cb550ba96b892dfeeabea982bf33b9237bcf9bbcd90f2e70a1)

set(MSGPACK_URL https://github.com/msgpack/msgpack-c/releases/download/c-6.0.0/msgpack-c-6.0.0.tar.gz)
set(MSGPACK_SHA256 3654f5e2c652dc52e0a993e270bb57d5702b262703f03771c152bba51602aeba)

# https://github.com/LuaJIT/LuaJIT/tree/v2.1
set(LUAJIT_URL https://github.com/LuaJIT/LuaJIT/archive/505e2c03de35e2718eef0d2d3660712e06dadf1f.tar.gz)
set(LUAJIT_SHA256 67c88399b901a22e9a236f4b77e6fe39af00f6b7144ce9dd6f51141d921f1076)

set(LUA_URL https://www.lua.org/ftp/lua-5.1.5.tar.gz)
set(LUA_SHA256 2640fc56a795f29d28ef15e13c34a47e223960b0240e8cb0a82d9b0738695333)

set(LUAROCKS_URL https://github.com/luarocks/luarocks/archive/v3.9.2.tar.gz)
set(LUAROCKS_SHA256 a0b36cd68586cd79966d0106bb2e5a4f5523327867995fd66bee4237062b3e3b)

set(UNIBILIUM_URL https://github.com/neovim/unibilium/archive/d72c3598e7ac5d1ebf86ee268b8b4ed95c0fa628.tar.gz)
set(UNIBILIUM_SHA256 9c4747c862ab5e3076dcf8fa8f0ea7a6b50f20ec5905618b9536655596797487)

set(LIBTERMKEY_URL https://github.com/neovim/deps/raw/aa004f1b2b6470a92363cba8e1cc1874141dacc4/opt/libtermkey-0.22.tar.gz)
set(LIBTERMKEY_SHA256 6945bd3c4aaa83da83d80a045c5563da4edd7d0374c62c0d35aec09eb3014600)

set(LIBVTERM_URL https://github.com/neovim/deps/raw/aa004f1b2b6470a92363cba8e1cc1874141dacc4/opt/libvterm-0.3.1.tar.gz)
set(LIBVTERM_SHA256 25a8ad9c15485368dfd0a8a9dca1aec8fea5c27da3fa74ec518d5d3787f0c397)

set(LUV_URL https://github.com/luvit/luv/archive/093a977b82077591baefe1e880d37dfa2730bd54.tar.gz)
set(LUV_SHA256 222b38b6425f0926218e14e7da81481fdde6f9660c1feac25a53e6fb52e886e6)

set(LUA_COMPAT53_URL https://github.com/keplerproject/lua-compat-5.3/archive/v0.9.tar.gz)
set(LUA_COMPAT53_SHA256 ad05540d2d96a48725bb79a1def35cf6652a4e2ec26376e2617c8ce2baa6f416)

# Windows only: cat.exe diff.exe tee.exe xxd.exe
set(CAT_URL https://github.com/neovim/deps/raw/21c5e8bdda33521a6ed497b315e03265a2785cbc/opt/cat.exe)
set(CAT_SHA256 93b8d307bb15af3968920bdea3beb869a49d166f9164853c58a4e6ffdcae61c6)
set(DIFF_URL https://github.com/neovim/deps/raw/21c5e8bdda33521a6ed497b315e03265a2785cbc/opt/diff.exe)
set(DIFF_SHA256 4ceceebc8150422c6d8d9a06c2e9686d5a5d90f1033f60ad92ab81fe810e2a28)
set(TEE_URL https://github.com/neovim/deps/raw/21c5e8bdda33521a6ed497b315e03265a2785cbc/opt/tee.exe)
set(TEE_SHA256 950eea4e17fa3a7e89fa2c55374037b5797b3f1a54fea1304634884ab42ec14d)
set(XXD_URL https://github.com/neovim/deps/raw/21c5e8bdda33521a6ed497b315e03265a2785cbc/opt/xxd.exe)
set(XXD_SHA256 7a581e3882d28161cc52850f9a11d634b3eaf2c029276f093c1ed4c90e45a10c)

set(WINGUI_URL https://github.com/equalsraf/neovim-qt/releases/download/v0.2.17/neovim-qt.zip)
set(WINGUI_SHA256 502e386eef677c2c2e0c11d8cbb27f3e12b4d96818369417e8da4129c4580c25)

set(WIN32YANK_X86_64_URL https://github.com/equalsraf/win32yank/releases/download/v0.1.1/win32yank-x64.zip)
set(WIN32YANK_X86_64_SHA256 247c9a05b94387a884b49d3db13f806b1677dfc38020f955f719be6902260cd6)

set(GETTEXT_URL https://ftp.gnu.org/pub/gnu/gettext/gettext-0.20.1.tar.gz)
set(GETTEXT_SHA256 66415634c6e8c3fa8b71362879ec7575e27da43da562c798a8a2f223e6e47f5c)

set(LIBICONV_URL https://ftp.gnu.org/pub/gnu/libiconv/libiconv-1.15.tar.gz)
set(LIBICONV_SHA256 ccf536620a45458d26ba83887a983b96827001e92a13847b45e4925cc8913178)

set(TREESITTER_C_URL https://github.com/tree-sitter/tree-sitter-c/archive/v0.20.2.tar.gz)
set(TREESITTER_C_SHA256 af66fde03feb0df4faf03750102a0d265b007e5d957057b6b293c13116a70af2 )
set(TREESITTER_LUA_URL https://github.com/MunifTanjim/tree-sitter-lua/archive/v0.0.17.tar.gz)
set(TREESITTER_LUA_SHA256 8963fd0a185d786c164dfca3824941c7eaec497ce49a3a0bc24bf753f5e0e59c)
set(TREESITTER_VIM_URL https://github.com/neovim/tree-sitter-vim/archive/v0.3.0.tar.gz)
set(TREESITTER_VIM_SHA256 403acec3efb7cdb18ff3d68640fc823502a4ffcdfbb71cec3f98aa786c21cbe2)
set(TREESITTER_VIMDOC_URL https://github.com/neovim/tree-sitter-vimdoc/archive/v2.0.0.tar.gz)
set(TREESITTER_VIMDOC_SHA256 1ff8f4afd3a9599dd4c3ce87c155660b078c1229704d1a254433e33794b8f274)
set(TREESITTER_QUERY_URL https://github.com/nvim-treesitter/tree-sitter-query/archive/v0.1.0.tar.gz)
set(TREESITTER_QUERY_SHA256 e2b806f80e8bf1c4f4e5a96248393fe6622fc1fc6189d6896d269658f67f914c)
set(TREESITTER_URL https://github.com/tree-sitter/tree-sitter/archive/v0.20.8.tar.gz)
set(TREESITTER_SHA256 6181ede0b7470bfca37e293e7d5dc1d16469b9485d13f13a605baec4a8b1f791)

if(USE_EXISTING_SRC_DIR)
  get_cmake_property(VARS VARIABLES)
  foreach (VAR ${VARS})
    if(VAR MATCHES "^.*URL$")
      unset(${VAR})
    endif()
  endforeach()
endif()

if(USE_BUNDLED_UNIBILIUM)
  include(BuildUnibilium)
endif()

if(USE_BUNDLED_LIBTERMKEY)
  include(BuildLibtermkey)
  if(USE_BUNDLED_UNIBILIUM)
    add_dependencies(libtermkey unibilium)
  endif()
endif()

if(USE_BUNDLED_LIBVTERM)
  include(BuildLibvterm)
endif()

if(USE_BUNDLED_LIBUV)
  include(BuildLibuv)
endif()

if(USE_BUNDLED_MSGPACK)
  include(BuildMsgpack)
endif()

if(USE_BUNDLED_LUAJIT)
  include(BuildLuajit)
endif()

if(USE_BUNDLED_LUA)
  include(BuildLua)
endif()

if(USE_BUNDLED_LUAROCKS)
  include(BuildLuarocks)
endif()

if(USE_BUNDLED_LUV)
  include(BuildLuv)
endif()

if(USE_BUNDLED_GETTEXT)
  include(BuildGettext)
endif()

if(USE_BUNDLED_LIBICONV)
  include(BuildLibiconv)
endif()

if(USE_BUNDLED_TS_PARSERS)
  include(BuildTreesitterParsers)
endif()

if(USE_BUNDLED_TS)
  include(BuildTreesitter)
endif()

if(WIN32)
  include(GetBinaryDeps)

  GetExecutable(TARGET cat)
  GetExecutable(TARGET diff)
  GetExecutable(TARGET tee)
  GetExecutable(TARGET xxd)

  if(USE_BUNDLED_NVIMQT)
    GetBinaryDep(TARGET wingui
      INSTALL_COMMAND ${CMAKE_COMMAND} -E copy_directory bin ${DEPS_BIN_DIR}
      COMMAND ${CMAKE_COMMAND} -E copy_directory share ${DEPS_INSTALL_DIR}/share)
  endif()

  GetBinaryDep(TARGET win32yank_X86_64
    INSTALL_COMMAND ${CMAKE_COMMAND} -E copy win32yank.exe ${DEPS_BIN_DIR})
endif()
