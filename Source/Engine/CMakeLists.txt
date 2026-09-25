project(Engine CXX)

file(GLOB SRC_FILES "Src/*.cpp")

if(LOW_MEMORY)
  list(FILTER SRC_FILES EXCLUDE REGEX "UnPath")
endif()

add_library(${PROJECT_NAME} ${LIB_TYPE} ${SRC_FILES})

if(TARGET_IS_DREAMCAST AND NOT (CMAKE_BUILD_TYPE STREQUAL "Debug"))
  # Pose decode/interpolation and frame transformation run for every visible
  # actor, even when later triangle culling rejects part of the mesh.
  set_source_files_properties(
    "Src/UnMesh.cpp"
    "Src/UnDCMesh.cpp"
    PROPERTIES COMPILE_FLAGS "-O3"
  )
endif()

target_include_directories(${PROJECT_NAME}
  PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/Inc
  PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/Src
)

target_link_libraries(${PROJECT_NAME} Core)
if(TARGET ZLIB::ZLIB)
  target_link_libraries(${PROJECT_NAME} ZLIB::ZLIB)
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE ENGINE_EXPORTS UPACKAGE_NAME=${PROJECT_NAME})
