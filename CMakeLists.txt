cmake_minimum_required(VERSION 3.15 FATAL_ERROR)

cmake_policy(SET CMP0091 NEW)

set(TARGET bf42plusplus)
project(${TARGET} VERSION 1.0 LANGUAGES C CXX)

option(BUILD_SHARED_LIBS "Build using shared libraries" ON)
option(BUILD_FOR_BF1942_R "Build bf42++ for BF1942_r.exe" OFF)

# GetAsyncKeyState is detected as keylogging behaviour by AVs
option(REDUCE_AV_SCORE "Remove functionality suspicious to AVs" ON)

if (${BUILD_FOR_BF1942_R})
  option(WITH_PROFILER "Build with Tracy profiler" OFF)
endif()


add_library(${TARGET} SHARED)

target_sources( ${TARGET} PRIVATE
  src/bfhook.cpp
  src/bf/console.cpp
  src/bf/gameevent.cpp
  src/bf/generic.cpp
  src/bf/input.cpp
  src/bf/net.cpp
  src/bf/object.cpp
  src/bf/renderer.cpp
  src/bf/skinning.cpp
  src/bf/stl.cpp
  src/bf/ui.cpp

  src/debug.cpp
  src/dllmain.cpp
  src/hooks.c
  src/replay.cpp
  src/settings.cpp
  src/util.cpp

  src/blake2b/blake2b-ref.c

  res/bf42plus.def
  res/bf42plus.rc
)

if (${WITH_PROFILER})
  target_sources( ${TARGET} PRIVATE
    src/profiling.cpp
    libs/tracy/TracyClient.cpp
  )

  target_compile_definitions( ${TARGET} PRIVATE
    TRACY_ENABLE
    TRACY_CALLSTACK = 3
    TRACY_NO_SYSTEM_TRACING
    TRACY_IGNORE_MEMORY_FAULTS
  )

endif()


set_target_properties( ${TARGET} PROPERTIES
  C_STANDARD_REQUIRED ON
  CXX_STANDARD 17

  CXX_STANDARD_REQUIRED ON
  CXX_STANDARD 20
)


target_compile_definitions( ${TARGET} PUBLIC
  UNICODE
  _UNICODE
)


if (${REDUCE_AV_SCORE})
  target_compile_definitions( ${TARGET} PUBLIC
    REDUCE_AV_SCORE
  )
endif()


target_compile_options( ${TARGET} PUBLIC
  /Zc:threadSafeInit
  /arch:IA32 # enforce x87 usage

  /wd4584 # multiple inheritance warnings
#  /wd4731 # frame pointer modified in inline assembly

#  /wd5105 # defined in .vcxproj
#  /wd4201 # defined in .vcxproj
)


target_link_libraries( ${TARGET} PUBLIC
  kernel32
  user32
  shell32
  Shlwapi
  Winmm
)


include(GNUInstallDirs)
target_include_directories( ${TARGET} PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/libs/fpng>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/libs/stb>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/libs/simpleini>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/libs/tracy/tracy>
)


add_executable( bf42plusplus_loader WIN32
  src/loader.cpp
  src/hooks.c
)

set_target_properties( bf42plusplus_loader PROPERTIES
  C_STANDARD_REQUIRED ON
  CXX_STANDARD 17

  CXX_STANDARD_REQUIRED ON
  CXX_STANDARD 20

  WIN32_EXECUTABLE TRUE
)


target_compile_definitions( bf42plusplus_loader PUBLIC
  UNICODE
  _UNICODE
)


if (NOT ${BUILD_FOR_BF1942_R})

  set_target_properties( ${TARGET} PROPERTIES
    RUNTIME_OUTPUT_NAME "bf42++"
  )

  set_target_properties( bf42plusplus_loader PROPERTIES
    RUNTIME_OUTPUT_NAME "bf42++"
  )

  set(BLACKSCREEN_TARGET_EXE_NAME "bf42++.exe" CACHE STRING "" FORCE)

else()

  set_target_properties( ${TARGET} PROPERTIES
    RUNTIME_OUTPUT_NAME "bf42_r++"
  )

  set_target_properties( bf42plusplus_loader PROPERTIES
    RUNTIME_OUTPUT_NAME "bf42_r++"
  )

  target_compile_definitions( ${TARGET} PUBLIC
    TARGET_BF1942_R
  )

  target_compile_definitions( bf42plusplus_loader PUBLIC
    TARGET_BF1942_R
  )

  set(BLACKSCREEN_TARGET_EXE_NAME "bf42_r++.exe" CACHE STRING "" FORCE)
endif()


add_subdirectory(BlackScreen)

if (NOT ${BUILD_FOR_BF1942_R})
  set_target_properties( BlackScreen PROPERTIES
    RUNTIME_OUTPUT_NAME "bf42++BlackScreen"
  )
else()
  set_target_properties( BlackScreen PROPERTIES
    RUNTIME_OUTPUT_NAME "bf42_r++BlackScreen"
  )
endif()
