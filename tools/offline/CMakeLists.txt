# Offline checks: generate every hull/domain shader variant against the real
# Fallout 4 landscape vertex/pixel signatures and compile them, and exercise the
# clipmap update shader. Neither needs the game.

set(OFFLINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(PLUGIN_SRC "${PROJECT_SOURCE_DIR}/src")

add_executable(DrawHookTest "${OFFLINE_DIR}/DrawHookTest.cpp" "${PLUGIN_SRC}/ShaderRegistry.cpp" "${PLUGIN_SRC}/ShaderReflection.cpp")
target_include_directories(DrawHookTest PRIVATE "${OFFLINE_DIR}" "${PLUGIN_SRC}")
target_compile_features(DrawHookTest PRIVATE cxx_std_23)
target_compile_definitions(DrawHookTest PRIVATE NMN_OFFLINE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(DrawHookTest PRIVATE "/utf-8" "/permissive-" "/Zc:preprocessor")
target_link_libraries(DrawHookTest PRIVATE d3d11.lib d3dcompiler.lib dxguid.lib)

add_executable(ShaderStateTest "${OFFLINE_DIR}/ShaderStateTest.cpp")
target_include_directories(ShaderStateTest PRIVATE "${OFFLINE_DIR}" "${PLUGIN_SRC}")
target_compile_features(ShaderStateTest PRIVATE cxx_std_23)
target_compile_definitions(ShaderStateTest PRIVATE NMN_OFFLINE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(ShaderStateTest PRIVATE "/utf-8" "/permissive-" "/Zc:preprocessor")
target_link_libraries(ShaderStateTest PRIVATE d3d11.lib d3dcompiler.lib dxguid.lib)

if(EXISTS "${OFFLINE_DIR}/ShaderGen.cpp")
    add_executable(ShaderGen "${OFFLINE_DIR}/ShaderGen.cpp")
    target_include_directories(ShaderGen PRIVATE "${OFFLINE_DIR}" "${PLUGIN_SRC}")
    target_compile_features(ShaderGen PRIVATE cxx_std_23)
    target_compile_definitions(ShaderGen PRIVATE NMN_OFFLINE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(ShaderGen PRIVATE "/utf-8" "/permissive-" "/Zc:preprocessor")
    target_link_libraries(ShaderGen PRIVATE d3d11.lib d3dcompiler.lib dxguid.lib)
endif()

add_executable(SettingsTest "${OFFLINE_DIR}/SettingsTest.cpp" "${PLUGIN_SRC}/Settings.cpp")
target_include_directories(SettingsTest PRIVATE "${OFFLINE_DIR}" "${PLUGIN_SRC}")
target_compile_features(SettingsTest PRIVATE cxx_std_23)
target_compile_definitions(SettingsTest PRIVATE NMN_OFFLINE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(SettingsTest PRIVATE "/utf-8" "/permissive-" "/Zc:preprocessor")

# Real CommonLib types are used to compile the production profile/type code. The
# test stubs only engine/field queries and throws if any of those paths are used.
add_executable(ProfilesTest "${OFFLINE_DIR}/ProfilesTest.cpp" "${PLUGIN_SRC}/SurfaceTypes.cpp" "${PLUGIN_SRC}/Settings.cpp"
    "${PLUGIN_SRC}/WinterLook.cpp")
target_include_directories(ProfilesTest PRIVATE "${PLUGIN_SRC}")
target_compile_features(ProfilesTest PRIVATE cxx_std_23)
target_compile_definitions(ProfilesTest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(ProfilesTest PRIVATE /utf-8 /EHsc /Gy /permissive- /Zc:preprocessor)
target_link_options(ProfilesTest PRIVATE /OPT:REF)
target_link_libraries(ProfilesTest PRIVATE CommonLibF4::CommonLibF4)

if(EXISTS "${OFFLINE_DIR}/StampSurfaceTest.cpp")
    add_executable(StampSurfaceTest "${OFFLINE_DIR}/StampSurfaceTest.cpp" "${PLUGIN_SRC}/DdsLoader.cpp"
        "${PLUGIN_SRC}/ShaderRegistry.cpp" "${PLUGIN_SRC}/ShaderReflection.cpp")
    target_include_directories(StampSurfaceTest PRIVATE "${OFFLINE_DIR}" "${PLUGIN_SRC}")
    target_compile_features(StampSurfaceTest PRIVATE cxx_std_23)
    target_compile_definitions(StampSurfaceTest PRIVATE NMN_OFFLINE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(StampSurfaceTest PRIVATE "/utf-8" "/permissive-" "/Zc:preprocessor")
    target_link_libraries(StampSurfaceTest PRIVATE d3d11.lib d3dcompiler.lib dxguid.lib)
endif()
