# Opt-in test helper. No install/deploy/package rules: never ships with the mod.
add_library(NMN_TerrainTestDriver SHARED Driver.cpp)
target_compile_features(NMN_TerrainTestDriver PRIVATE cxx_std_23)
target_compile_definitions(NMN_TerrainTestDriver PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(NMN_TerrainTestDriver PRIVATE /utf-8 /EHsc /permissive- /Zc:preprocessor)
target_include_directories(NMN_TerrainTestDriver PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(NMN_TerrainTestDriver PRIVATE CommonLibF4::CommonLibF4)

add_library(NMN_PrivateInputGuard SHARED PrivateInputGuard.cpp)
target_compile_features(NMN_PrivateInputGuard PRIVATE cxx_std_23)
target_compile_definitions(NMN_PrivateInputGuard PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(NMN_PrivateInputGuard PRIVATE /utf-8 /EHsc /permissive- /Zc:preprocessor)
target_link_libraries(NMN_PrivateInputGuard PRIVATE CommonLibF4::CommonLibF4 user32)
