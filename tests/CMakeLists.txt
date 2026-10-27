# Engine tests: console programs linked against the engine's own objects
# (faf_engine, src/CMakeLists.txt). Not part of the default build:
#
#   cmake --build --preset clang-x64-release --target faf_net_tests
#   ctest --test-dir build/clang-x64-release

add_executable(faf_net_tests EXCLUDE_FROM_ALL net/NetRobustnessTests.cpp)
target_link_libraries(faf_net_tests PRIVATE faf_engine)

add_test(NAME net_robustness COMMAND faf_net_tests)

# Dynamic light pool (src/moho/render/lights): pure C++, compiled on its own, no engine objects.
#
#   cmake --build --preset clang-x64-release --target faf_light_tests
add_executable(faf_light_tests EXCLUDE_FROM_ALL lights/LightCollectorTests.cpp lights/LightGridTests.cpp lights/LightEffectPatchTests.cpp lights/LightTextureTests.cpp lights/LightMeshEmissionTests.cpp
               ${CMAKE_SOURCE_DIR}/src/moho/render/lights/LightCollector.cpp
               ${CMAKE_SOURCE_DIR}/src/moho/render/lights/LightGrid.cpp
               ${CMAKE_SOURCE_DIR}/src/moho/render/lights/LightEffectPatch.cpp
               ${CMAKE_SOURCE_DIR}/src/moho/render/lights/LightTexture.cpp
               ${CMAKE_SOURCE_DIR}/src/moho/render/lights/LightMeshEmission.cpp)
target_include_directories(faf_light_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(faf_light_tests PRIVATE cxx_std_20)

add_test(NAME dynamic_lights COMMAND faf_light_tests)

# Shadow filter effect patch (src/moho/render/shadow): pure text in, text out. With FAF_STOCK_FX_DIR set it patches FAF's stock effects.
#
#   cmake --build --preset clang-x64-release --target faf_shadow_tests
add_executable(faf_shadow_tests EXCLUDE_FROM_ALL shadow/ShadowEffectPatchTests.cpp
               ${CMAKE_SOURCE_DIR}/src/moho/render/shadow/ShadowEffectPatch.cpp
               ${CMAKE_SOURCE_DIR}/src/moho/render/lights/LightEffectPatch.cpp)
target_include_directories(faf_shadow_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(faf_shadow_tests PRIVATE cxx_std_20)

add_test(NAME shadow_filter_patch COMMAND faf_shadow_tests)
