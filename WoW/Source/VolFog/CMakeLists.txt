# CoAVolFog (https://github.com/jealous-sound/coa-vfog, GPL-3.0) built into the client instead of attaching as a DLL.
# Windows only: it draws through the Direct3D 9 device and its shaders are compiled with fxc from the Windows SDK.

file(GLOB FXC_CANDIDATES "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/10.*/x86/fxc.exe")
list(SORT FXC_CANDIDATES COMPARE NATURAL ORDER DESCENDING)
list(GET FXC_CANDIDATES 0 FXC_DEFAULT)
set(FXC "${FXC_DEFAULT}" CACHE FILEPATH "HLSL compiler (fxc.exe)")
if(NOT EXISTS "${FXC}")
    message(FATAL_ERROR "CoAVolFog needs fxc.exe from the Windows SDK: install the SDK, set -DFXC=<path>, or configure with -DWOWDECOMP_VOLFOG=OFF")
endif()

set(SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT})

set(VF_INCLUDES ${SHADER_DIR}/vf_common.hlsli ${SHADER_DIR}/vf_integrate.hlsli
                ${SHADER_DIR}/vf_local_lights.hlsli ${SHADER_DIR}/vf_density_variation.hlsli
                ${SHADER_DIR}/vf_authored_noise.hlsli ${SHADER_DIR}/vf_composite.hlsli
                ${SHADER_DIR}/vf_sample_split.hlsli)
set(VW_FFT_INCLUDES ${SHADER_DIR}/vw_fft_common.hlsli)
set(VW_INCLUDES ${SHADER_DIR}/vf_common.hlsli ${SHADER_DIR}/vw_constants.hlsli ${SHADER_DIR}/vw_surface.hlsli
                ${SHADER_DIR}/vw_optics.hlsli ${SHADER_DIR}/vw_reflection.hlsli)

set(SHADER_HEADERS)
function(vf_shader name source profile)
    if(source MATCHES "^vw_fft_")
        set(includes ${VW_FFT_INCLUDES})
    elseif(source MATCHES "^vw_")
        set(includes ${VW_INCLUDES})
    else()
        set(includes ${VF_INCLUDES})
    endif()
    set(out ${SHADER_OUT}/${name}.h)
    add_custom_command(
        OUTPUT ${out}
        COMMAND "${FXC}" /nologo /O3 /T ${profile} /E main /Vn g_${name} ${ARGN}
                /Fh ${out} /Fc ${SHADER_OUT}/${name}.asm ${SHADER_DIR}/${source}
        DEPENDS ${SHADER_DIR}/${source} ${includes}
        COMMENT "fxc ${name}"
        VERBATIM)
    set(SHADER_HEADERS ${SHADER_HEADERS} ${out} PARENT_SCOPE)
endfunction()

vf_shader(vs_fullscreen vf_fullscreen_vs.hlsl vs_3_0)
vf_shader(ps_march_low vf_march.hlsl ps_3_0 /DSTEPS=16 /DLOCAL_LIGHTS=0)
vf_shader(ps_march_mid vf_march.hlsl ps_3_0 /DSTEPS=24 /DLOCAL_LIGHTS=0)
vf_shader(ps_march_high vf_march.hlsl ps_3_0 /DSTEPS=32 /DLOCAL_LIGHTS=0)
vf_shader(ps_lit_march_low vf_march.hlsl ps_3_0 /DSTEPS=16 /DLOCAL_LIGHTS=1)
vf_shader(ps_lit_march_mid vf_march.hlsl ps_3_0 /DSTEPS=24 /DLOCAL_LIGHTS=1)
vf_shader(ps_lit_march_high vf_march.hlsl ps_3_0 /DSTEPS=32 /DLOCAL_LIGHTS=1)
vf_shader(ps_noisy_march_low vf_march.hlsl ps_3_0 /DSTEPS=16 /DLOCAL_LIGHTS=0 /DSAMPLES_AUTHORED_NOISE=1)
vf_shader(ps_noisy_march_mid vf_march.hlsl ps_3_0 /DSTEPS=24 /DLOCAL_LIGHTS=0 /DSAMPLES_AUTHORED_NOISE=1)
vf_shader(ps_noisy_march_high vf_march.hlsl ps_3_0 /DSTEPS=32 /DLOCAL_LIGHTS=0 /DSAMPLES_AUTHORED_NOISE=1)
vf_shader(ps_lit_noisy_march_low vf_march.hlsl ps_3_0 /DSTEPS=16 /DLOCAL_LIGHTS=1 /DSAMPLES_AUTHORED_NOISE=1)
vf_shader(ps_lit_noisy_march_mid vf_march.hlsl ps_3_0 /DSTEPS=24 /DLOCAL_LIGHTS=1 /DSAMPLES_AUTHORED_NOISE=1)
vf_shader(ps_lit_noisy_march_high vf_march.hlsl ps_3_0 /DSTEPS=32 /DLOCAL_LIGHTS=1 /DSAMPLES_AUTHORED_NOISE=1)
vf_shader(ps_density_probe vf_density_probe.hlsl ps_3_0)
vf_shader(ps_temporal vf_temporal.hlsl ps_3_0)
vf_shader(ps_history_depth vf_history_depth.hlsl ps_3_0)
vf_shader(ps_composite_low vf_composite.hlsl ps_3_0 /DSTEPS=16 /DLOCAL_LIGHTS=0)
vf_shader(ps_composite_mid vf_composite.hlsl ps_3_0 /DSTEPS=24 /DLOCAL_LIGHTS=0)
vf_shader(ps_composite_high vf_composite.hlsl ps_3_0 /DSTEPS=32 /DLOCAL_LIGHTS=0)
vf_shader(ps_noisy_composite_low vf_composite.hlsl ps_3_0 /DSTEPS=16 /DLOCAL_LIGHTS=0 /DSAMPLES_AUTHORED_NOISE=1)
vf_shader(ps_noisy_composite_mid vf_composite.hlsl ps_3_0 /DSTEPS=24 /DLOCAL_LIGHTS=0 /DSAMPLES_AUTHORED_NOISE=1)
vf_shader(ps_noisy_composite_high vf_composite.hlsl ps_3_0 /DSTEPS=32 /DLOCAL_LIGHTS=0 /DSAMPLES_AUTHORED_NOISE=1)
vf_shader(ps_lit_composite_low vf_composite.hlsl ps_3_0 /DSTEPS=16 /DLOCAL_LIGHTS=1)
vf_shader(ps_lit_composite_mid vf_composite.hlsl ps_3_0 /DSTEPS=24 /DLOCAL_LIGHTS=1)
vf_shader(ps_lit_composite_high vf_composite.hlsl ps_3_0 /DSTEPS=32 /DLOCAL_LIGHTS=1)
vf_shader(ps_split_composite_low vf_composite_split.hlsl ps_3_0 /DSTEPS=16 /DLOCAL_LIGHTS=0)
vf_shader(ps_split_composite_mid vf_composite_split.hlsl ps_3_0 /DSTEPS=24 /DLOCAL_LIGHTS=0)
vf_shader(ps_split_composite_high vf_composite_split.hlsl ps_3_0 /DSTEPS=32 /DLOCAL_LIGHTS=0)
vf_shader(ps_noisy_split_composite_low vf_composite_split.hlsl ps_3_0 /DSTEPS=16 /DLOCAL_LIGHTS=0
          /DSAMPLES_AUTHORED_NOISE=1)
vf_shader(ps_noisy_split_composite_mid vf_composite_split.hlsl ps_3_0 /DSTEPS=24 /DLOCAL_LIGHTS=0
          /DSAMPLES_AUTHORED_NOISE=1)
vf_shader(ps_noisy_split_composite_high vf_composite_split.hlsl ps_3_0 /DSTEPS=32 /DLOCAL_LIGHTS=0
          /DSAMPLES_AUTHORED_NOISE=1)
vf_shader(ps_lit_split_composite_low vf_composite_split.hlsl ps_3_0 /DSTEPS=16 /DLOCAL_LIGHTS=1)
vf_shader(ps_lit_split_composite_mid vf_composite_split.hlsl ps_3_0 /DSTEPS=24 /DLOCAL_LIGHTS=1)
vf_shader(ps_lit_split_composite_high vf_composite_split.hlsl ps_3_0 /DSTEPS=32 /DLOCAL_LIGHTS=1)
vf_shader(ps_silhouette_mask vf_silhouette_mask.hlsl ps_3_0 /DLOCAL_LIGHTS=0)
vf_shader(ps_ray_mask vf_ray_mask.hlsl ps_3_0)
vf_shader(ps_ray_blur vf_ray_blur.hlsl ps_3_0)
vf_shader(ps_ray_composite vf_ray_composite.hlsl ps_3_0 /DSTEPS=16 /DLOCAL_LIGHTS=0)
vf_shader(ps_probe vf_probe.hlsl ps_3_0)
vf_shader(ps_depth_check vf_depth_check.hlsl ps_3_0)
vf_shader(ps_grade vp_grade.hlsl ps_3_0)
vf_shader(vs_client_m2_fog ../tests/shaders/client_m2_fog_vs.hlsl vs_3_0)
vf_shader(ps_client_m2_fog ../tests/shaders/client_m2_fog_ps.hlsl ps_3_0)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/WaterFftShaders.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/WaterShaders.cmake)
add_custom_target(vf_shaders DEPENDS ${SHADER_HEADERS})

include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.tar.gz
    URL_HASH SHA256=21d8a0a565e85dce943e375db00812c2f3f0ab21f3f0f7964e364a63422d7f99)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx9.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui
    PUBLIC IMGUI_DISABLE_DEMO_WINDOWS IMGUI_DISABLE_DEBUG_TOOLS
    PRIVATE IMGUI_IMPL_WIN32_DISABLE_GAMEPAD IMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS
            IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS WIN32_LEAN_AND_MEAN NOMINMAX)

add_library(VolFog STATIC
    src/client_native.cpp
    src/client_ripple_sprites.cpp
    src/config.cpp
    src/d3d9_wrap.cpp
    src/engine.cpp
    src/engine_actors.cpp
    src/engine_lights.cpp
    src/fog_data.cpp
    src/fog_model.cpp
    src/forever_look.cpp
    src/gpu_timing.cpp
    src/grading_renderer.cpp
    src/hooks.cpp
    src/log.cpp
    src/msaa_depth.cpp
    src/native_main.cpp
    src/noise_volume.cpp
    src/overlay.cpp
    src/renderer.cpp
    src/settings_panel.cpp
    src/status_log.cpp
    src/transparent_fog.cpp
    src/water_contacts.cpp
    src/water_data.cpp
    src/water_fft.cpp
    src/water_renderer.cpp
    src/water_ripples.cpp
    src/water_spectrum.cpp)
add_dependencies(VolFog vf_shaders)
target_include_directories(VolFog PRIVATE src ${SHADER_OUT} PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_definitions(VolFog PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(VolFog PRIVATE /fp:fast /GS)
endif()
target_link_libraries(VolFog PRIVATE imgui kernel32 user32)

# The files CoAVolFog reads at start-up, placed next to the client executable.
set(VOLFOG_RUNTIME_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/CoAVolFog.ini
    ${CMAKE_CURRENT_SOURCE_DIR}/data/fogdata.bin
    ${CMAKE_CURRENT_SOURCE_DIR}/data/waterdata.bin
    CACHE INTERNAL "")

function(volfog_copy_runtime_files target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${VOLFOG_RUNTIME_FILES} $<TARGET_FILE_DIR:${target}>)
endfunction()
