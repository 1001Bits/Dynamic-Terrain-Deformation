cmake_minimum_required(VERSION 3.23)
set(NAME "NMN_DeformableTerrain")
set(VERSION 1.0.0)

# ── Project setup (based on Daytripper4 multi-runtime pattern) ──────────────
project(
    ${NAME}
    VERSION ${VERSION}
    LANGUAGES CXX
)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if("${PROJECT_SOURCE_DIR}" STREQUAL "${PROJECT_BINARY_DIR}")
    message(FATAL_ERROR "in-source builds are not allowed")
endif()

include(cmake/common.cmake)

add_subdirectory(src)
include(cmake/packaging.cmake)

option(BUILD_OFFLINE_TOOLS "Build the offline shader and deformation checks" ON)
if(BUILD_OFFLINE_TOOLS)
    add_subdirectory(tools/offline)
endif()

option(BUILD_INGAME_DRIVER "Build the local-only native console test helper" OFF)
if(BUILD_INGAME_DRIVER)
    add_subdirectory(tools/ingame/driver)
endif()

# Disable C++ modules in MSVC IDE (avoids STL modules build issues)
if(MSVC_VERSION GREATER_EQUAL 1936 AND MSVC_IDE)
    file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/Directory.Build.props" CONTENT [==[
<Project>
  <ItemDefinitionGroup>
    <ClCompile>
      <BuildStlModules>false</BuildStlModules>
    </ClCompile>
  </ItemDefinitionGroup>
</Project>
]==] @ONLY)
endif()
