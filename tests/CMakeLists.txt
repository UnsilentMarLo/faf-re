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

# Swap-chain decisions (src/gpg/gal/backends/dxgi/DxgiSwapChainPolicy.h): header-only, no device.
#
#   cmake --build --preset clang-x64-release --target faf_dxgi_tests
add_executable(faf_dxgi_tests EXCLUDE_FROM_ALL dxgi/SwapChainPolicyTests.cpp)
target_include_directories(faf_dxgi_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(faf_dxgi_tests PRIVATE cxx_std_20)
target_link_libraries(faf_dxgi_tests PRIVATE dxgi)

add_test(NAME dxgi_swap_chain_policy COMMAND faf_dxgi_tests)

# Particle effect patch (src/moho/particles/ParticleEffectPatch): pure text, compiled on its own, no engine objects. FAF_STOCK_FX_DIR (a folder
# with FAF's particle.fx) switches on the check of the real file.
#
#   cmake --build --preset clang-x64-release --target faf_particle_tests
add_executable(faf_particle_tests EXCLUDE_FROM_ALL particles/ParticleEffectPatchTests.cpp
               ${CMAKE_SOURCE_DIR}/src/moho/particles/ParticleEffectPatch.cpp)
target_include_directories(faf_particle_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(faf_particle_tests PRIVATE cxx_std_20)

add_test(NAME particle_effect_patch COMMAND faf_particle_tests)
