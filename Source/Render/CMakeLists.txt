project(Render CXX)

set(SRC_FILES
  "Src/UnLight.cpp"
  "Src/UnMeshRn.cpp"
  "Src/UnRandom.cpp"
  "Src/UnRender.cpp"
  "Src/UnSoftLn.cpp"
  "Src/UnSpan.cpp"
  "Src/UnSprite.cpp"
  "Src/UnTest.cpp"
  "Src/Render.cpp"
)

add_library(${PROJECT_NAME} ${LIB_TYPE} ${SRC_FILES})

set(DREAMCAST_TRAVERSAL_OPT "O3" CACHE STRING "OccludeBsp optimization: O3, O2 or Os")
set(DREAMCAST_MESH_DRAW_OPT "O3" CACHE STRING "DrawMesh optimization: O3, O2 or Os")
foreach(OPTION DREAMCAST_TRAVERSAL_OPT DREAMCAST_MESH_DRAW_OPT)
  if(NOT "${${OPTION}}" MATCHES "^(O3|O2|Os)$")
    message(FATAL_ERROR "${OPTION} must be O3, O2 or Os")
  endif()
endforeach()
if(TARGET_IS_DREAMCAST)
  target_compile_definitions(${PROJECT_NAME} PRIVATE
    DC_TRAVERSAL_OPT="${DREAMCAST_TRAVERSAL_OPT}"
    DC_MESH_DRAW_OPT="${DREAMCAST_MESH_DRAW_OPT}")
endif()

if(TARGET_IS_DREAMCAST AND NOT (CMAKE_BUILD_TYPE STREQUAL "Debug"))
  # Keep the large hot loops at -O3 without growing every engine object.
  # UnLight already required an override: compiling it with -Os breaks lightmaps.
  set_source_files_properties(
    "Src/UnLight.cpp"
    "Src/UnMeshRn.cpp"
    "Src/UnRender.cpp"
    "Src/UnSpan.cpp"
    PROPERTIES COMPILE_FLAGS "-O3"
  )
endif()

target_include_directories(${PROJECT_NAME}
  PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/Inc
  PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/Src
)

target_link_libraries(${PROJECT_NAME} Engine Core)
if(TARGET ZLIB::ZLIB)
  target_link_libraries(${PROJECT_NAME} ZLIB::ZLIB)
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE RENDER_EXPORTS UPACKAGE_NAME=${PROJECT_NAME})
