project(DCUtil)

set(SRC_FILES "Src/Main.cpp" "Src/Texture.cpp" "Src/Resources.cpp" "Src/SplitBsp.cpp")

add_executable(${PROJECT_NAME} ${SRC_FILES})

target_include_directories(${PROJECT_NAME}
  PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/Inc
  PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/Src
)

if(BUILD_STATIC)
  set(LINK_PACKAGES ${INSTALL_TARGETS})
  list(REMOVE_ITEM LINK_PACKAGES ${PROJECT_NAME})
  list(REMOVE_ITEM LINK_PACKAGES Unreal)
  target_link_libraries(${PROJECT_NAME} -Wl,--whole-archive ${LINK_PACKAGES} -Wl,--no-whole-archive)
else()
  target_link_libraries(${PROJECT_NAME} Engine Core)
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE DCUTIL_EXPORTS UPACKAGE_NAME=${PROJECT_NAME})

# The map replay harness may load any configured native package dynamically.
# Rebuild them with Core so registration checks never use stale libraries.
if(DC_RESOURCE_COOKER AND NOT BUILD_STATIC)
  foreach(PACKAGE IN LISTS INSTALL_TARGETS)
    if(TARGET ${PACKAGE} AND NOT PACKAGE STREQUAL "DCUtil" AND NOT PACKAGE STREQUAL "Unreal")
      add_dependencies(${PROJECT_NAME} ${PACKAGE})
    endif()
  endforeach()
endif()
