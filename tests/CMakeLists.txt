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

# Ambient occlusion (src/gpg/gal/backends/common/AmbientOcclusion.h): the arithmetic, and the shaders run on a software Direct3D 11 device (WARP).
#
#   cmake --build --preset clang-x64-release --target faf_ao_tests
add_executable(faf_ao_tests EXCLUDE_FROM_ALL ao/AmbientOcclusionTests.cpp)
target_include_directories(faf_ao_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(faf_ao_tests PRIVATE cxx_std_20)
if(WIN32)
  target_link_libraries(faf_ao_tests PRIVATE d3d11 d3dcompiler)
endif()

add_test(NAME ambient_occlusion COMMAND faf_ao_tests)
