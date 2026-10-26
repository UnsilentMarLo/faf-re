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
add_executable(faf_light_tests EXCLUDE_FROM_ALL lights/LightCollectorTests.cpp
               ${CMAKE_SOURCE_DIR}/src/moho/render/lights/LightCollector.cpp)
target_include_directories(faf_light_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(faf_light_tests PRIVATE cxx_std_20)

add_test(NAME dynamic_lights COMMAND faf_light_tests)
