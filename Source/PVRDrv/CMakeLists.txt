project(PVRDrv C CXX)

if(NOT TARGET_IS_DREAMCAST)
  message(FATAL_ERROR "PVRDrv is for the Dreamcast only.")
endif()

set(SRC_FILES "PVRDrv.cpp")

add_library(${PROJECT_NAME} ${LIB_TYPE} ${SRC_FILES})

if(NOT (CMAKE_BUILD_TYPE STREQUAL "Debug"))
  # This source-level flag follows the global -O2/-Os flags from Source/CMakeLists.txt.
  set_source_files_properties("PVRDrv.cpp" PROPERTIES COMPILE_FLAGS "-O3")
endif()

target_include_directories(${PROJECT_NAME}
  PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/Inc
)

target_include_directories(${PROJECT_NAME}
  PRIVATE
  ${KOS_BASE}/include
)

target_link_libraries(${PROJECT_NAME} Render Engine Core)

# sh4zam supplies the SH-4 fast paths (fipr / fsrra / fsca / ftrv via XMTRX).
# kos-ports include and lib directories are already on the global paths.
find_library(SH4ZAM_LIBRARY NAMES sh4zam
  PATHS "${KOS_BASE}/../kos-ports/lib" NO_DEFAULT_PATH)
if(NOT SH4ZAM_LIBRARY)
  message(FATAL_ERROR "libsh4zam not found in ${KOS_BASE}/../kos-ports/lib")
endif()
target_link_libraries(${PROJECT_NAME} ${SH4ZAM_LIBRARY})
if(TARGET ZLIB::ZLIB)
  target_link_libraries(${PROJECT_NAME} ZLIB::ZLIB)
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE GLDCDRV_EXPORTS UPACKAGE_NAME=${PROJECT_NAME})
