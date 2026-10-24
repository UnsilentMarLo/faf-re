// The erosion kernels (gpu/erosion_*.hlsl) as C++ for the CPU emulator: included by tests/EdGpuTest.cpp (every generated kernel needs an emulation) and tests/EdGpuErosionTest.cpp.
// Include AFTER hlsl_cpp_shim.h and tests/EdGpuEmu.h, at global scope.
#include "moho/editor/gpu/common.hlsli"
#include "moho/editor/gpu/erosion_common.hlsli"
#include "moho/editor/gpu/erosion_drop_params.hlsli"
#include "moho/editor/gpu/erosion_hydro_params.hlsli"
#include "moho/editor/gpu/erosion_thermal_params.hlsli"

EMU_KERNEL_BEGIN(k_erosion_blend)
#include "moho/editor/gpu/erosion_blend.hlsl"
EMU_KERNEL_END(erosion_blend)

EMU_KERNEL_BEGIN(k_erosion_copy)
#include "moho/editor/gpu/erosion_copy.hlsl"
EMU_KERNEL_END(erosion_copy)

EMU_KERNEL_BEGIN(k_erosion_delta)
#include "moho/editor/gpu/erosion_delta.hlsl"
EMU_KERNEL_END(erosion_delta)

EMU_KERNEL_BEGIN(k_erosion_drop)
#include "moho/editor/gpu/erosion_drop.hlsl"
EMU_KERNEL_END(erosion_drop)

EMU_KERNEL_BEGIN(k_erosion_drop_resolve)
#include "moho/editor/gpu/erosion_drop_resolve.hlsl"
EMU_KERNEL_END(erosion_drop_resolve)

EMU_KERNEL_BEGIN(k_erosion_hydro_flux)
#include "moho/editor/gpu/erosion_hydro_flux.hlsl"
EMU_KERNEL_END(erosion_hydro_flux)

EMU_KERNEL_BEGIN(k_erosion_hydro_settle)
#include "moho/editor/gpu/erosion_hydro_settle.hlsl"
EMU_KERNEL_END(erosion_hydro_settle)

EMU_KERNEL_BEGIN(k_erosion_hydro_transport)
#include "moho/editor/gpu/erosion_hydro_transport.hlsl"
EMU_KERNEL_END(erosion_hydro_transport)

EMU_KERNEL_BEGIN(k_erosion_hydro_water)
#include "moho/editor/gpu/erosion_hydro_water.hlsl"
EMU_KERNEL_END(erosion_hydro_water)

EMU_KERNEL_BEGIN(k_erosion_steer)
#include "moho/editor/gpu/erosion_steer.hlsl"
EMU_KERNEL_END(erosion_steer)

EMU_KERNEL_BEGIN(k_erosion_thermal_a)
#include "moho/editor/gpu/erosion_thermal_a.hlsl"
EMU_KERNEL_END(erosion_thermal_a)

EMU_KERNEL_BEGIN(k_erosion_thermal_b)
#include "moho/editor/gpu/erosion_thermal_b.hlsl"
EMU_KERNEL_END(erosion_thermal_b)
