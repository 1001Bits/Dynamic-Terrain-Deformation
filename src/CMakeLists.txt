# ── CommonLibF4 multi-runtime source build ───────────────────────────────────

macro(find_commonlib_path)
    if(CommonLibName AND NOT ${CommonLibName} STREQUAL "")
        find_path(CommonLibPath
            include/REL/Relocation.h
            PATHS ${ROOT_DIR}/external/${CommonLibName}/${CommonLibName}/)
        if(${CommonLibPath} STREQUAL "CommonLibPath-NOTFOUND")
            set_from_environment(${CommonLibName}Path)
            set(CommonLibPath ${${CommonLibName}Path})
        endif()
    endif()
endmacro()

set(CommonLibName "CommonLibF4")
set_root_directory()

find_commonlib_path()
message(STATUS "Building ${PROJECT_NAME} ${PROJECT_VERSION} with ${CommonLibName} at ${CommonLibPath}.")

set(SOURCE_DIR "${ROOT_DIR}/src")
file(GLOB SOURCE_FILES CONFIGURE_DEPENDS
    "${SOURCE_DIR}/*.cpp"
    "${SOURCE_DIR}/*.h"
)

source_group(TREE "${ROOT_DIR}" FILES ${SOURCE_FILES})

set(VERSION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/src/Plugin.h")
configure_file(
    "${ROOT_DIR}/cmake/Plugin.h.in"
    "${VERSION_HEADER}"
    @ONLY
)
source_group("src" FILES "${VERSION_HEADER}")

configure_file(
    "${ROOT_DIR}/cmake/version.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
    @ONLY
)

add_library(
    "${PROJECT_NAME}"
    SHARED
    ${SOURCE_FILES}
    "${VERSION_HEADER}"
    "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
    "${ROOT_DIR}/.clang-format"
    "${ROOT_DIR}/.editorconfig"
)

target_compile_features("${PROJECT_NAME}" PRIVATE cxx_std_23)

if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
    target_compile_options("${PROJECT_NAME}" PRIVATE
        "/EHa" "/sdl" "/utf-8" "/Zi"
        "/permissive-" "/Zc:preprocessor"
        "/wd4324"
        "$<$<CONFIG:DEBUG>:>"
        "$<$<CONFIG:RELEASE>:/Zc:inline;/JMC-;/Ob3>"
    )
    target_link_options("${PROJECT_NAME}" PRIVATE
        "$<$<CONFIG:DEBUG>:/INCREMENTAL;/OPT:NOREF;/OPT:NOICF>"
        "$<$<CONFIG:RELEASE>:/INCREMENTAL:NO;/OPT:REF;/OPT:ICF;/DEBUG:FULL>"
    )
endif()

target_include_directories("${PROJECT_NAME}" PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/src"
    "${SOURCE_DIR}"
)

if(DEFINED CommonLibPath AND NOT ${CommonLibPath} STREQUAL "" AND IS_DIRECTORY ${CommonLibPath})
    add_subdirectory(${CommonLibPath} ${CommonLibName})
else()
    message(FATAL_ERROR "Variable ${CommonLibName}Path is not set or in external/.")
endif()

find_package(fmt REQUIRED CONFIG)
find_package(spdlog REQUIRED CONFIG)
find_package(mmio REQUIRED CONFIG)
find_package(xbyak REQUIRED CONFIG)

add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
add_compile_definitions(_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING)

target_link_libraries("${PROJECT_NAME}" PRIVATE
    CommonLibF4::CommonLibF4
    fmt::fmt
    mmio::mmio
    spdlog::spdlog
    xbyak::xbyak
    d3d11.lib
    d3dcompiler.lib
    dxgi.lib
    dxguid.lib
)

target_compile_definitions("${PROJECT_NAME}" PRIVATE F4SE_SUPPORT_XBYAK WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_definitions(CommonLibF4 PUBLIC F4SE_SUPPORT_XBYAK _SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING)

target_precompile_headers("${PROJECT_NAME}" PRIVATE "${SOURCE_DIR}/PCH.h")

handle_data_files(
    FILES
    "$<TARGET_FILE:${PROJECT_NAME}>"
    "${ROOT_DIR}/data/Data/F4SE/Plugins/${PROJECT_NAME}.ini"
    DESTINATION "F4SE/Plugins"
)
install(
    DIRECTORY "${ROOT_DIR}/data/Data/Textures"
    DESTINATION "."
    COMPONENT "main"
)

# ── Optional deploy ─────────────────────────────────────────────────────────
# Off by default: the 1.10.163 install is shared with other plugins under test.
# Configure with -DNMN_DEPLOY=ON to copy the DLL (and on first deploy the INI and
# textures) into the targets below after every build.
option(NMN_DEPLOY "Copy the plugin into the MO2 mod folder / game Data after building" OFF)
set(NMN_DEPLOY_TARGETS
    "C:/Games/MO2/mods/${PROJECT_NAME}"
    "C:/Games/Fallout.4 1.10.163/Data"
    CACHE STRING "Deploy targets (each receives F4SE/Plugins and Textures)")
if(NMN_DEPLOY)
    foreach(DEPLOY_TARGET ${NMN_DEPLOY_TARGETS})
        if(EXISTS "${DEPLOY_TARGET}")
            message("Adding deployment target ${DEPLOY_TARGET}.")
            add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${DEPLOY_TARGET}/F4SE/Plugins/"
                COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}> "${DEPLOY_TARGET}/F4SE/Plugins/"
                COMMAND ${CMAKE_COMMAND} -E copy_directory "${ROOT_DIR}/data/Data/Textures" "${DEPLOY_TARGET}/Textures")
            if(NOT EXISTS "${DEPLOY_TARGET}/F4SE/Plugins/${PROJECT_NAME}.ini")
                add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy "${ROOT_DIR}/data/Data/F4SE/Plugins/${PROJECT_NAME}.ini" "${DEPLOY_TARGET}/F4SE/Plugins/")
            endif()
        else()
            message("Skipping deployment target ${DEPLOY_TARGET} (does not exist).")
        endif()
    endforeach()
endif()

# Package on every build, including INI/texture-only edits. The script stages only
# production files in a new directory so leftovers and test helpers cannot ship.
set(TARGET_ZIP "${PROJECT_NAME}-${PROJECT_VERSION}.7z")
add_custom_target(release-archive ALL
    COMMAND ${CMAKE_COMMAND}
        "-DBUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        "-DDATA_DIR=${ROOT_DIR}/data/Data"
        "-DPLUGIN_FILE=$<TARGET_FILE:${PROJECT_NAME}>"
        "-DPLUGIN_NAME=${PROJECT_NAME}"
        "-DARCHIVE_NAME=${TARGET_ZIP}"
        -P "${ROOT_DIR}/cmake/package-release.cmake"
    DEPENDS ${PROJECT_NAME}
    VERBATIM)
