# GLSL -> SPIR-V -> (DXIL, MSL) at build time: shaders/*.vert|.frag -> spv/<name>.inc, a C++ include with the shader in every format
# the SDL_GPU backends ask for (embed_shader.cmake). glslc comes with vcpkg's shaderc port (tools/shaderc) or the Vulkan SDK;
# shadercross (sdl3-shadercross port) converts SPIR-V to DXIL and MSL. Without it only SPIR-V (Vulkan) is embedded.
find_program(OC_GLSLC glslc
  HINTS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/shaderc" "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin"
  REQUIRED)
find_program(OC_SHADERCROSS shadercross
  HINTS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/sdl3-shadercross")
set(OC_SHADER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(OC_SPV_DIR ${CMAKE_CURRENT_BINARY_DIR}/spv)
file(MAKE_DIRECTORY ${OC_SPV_DIR})
set(OC_SHADER_FORMATS SDL_GPU_SHADERFORMAT_SPIRV)
if(OC_SHADERCROSS)
  set(OC_SHADER_FORMATS "SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL")
endif()
set(OC_SHADER_INCS)
# out = include file name (sprite.vert.inc); src = GLSL file; extra arguments = glslc options (e.g. -DAFFINE).
function(oc_shader out src)
  string(REGEX REPLACE "\\.inc$" "" base ${out})
  string(REPLACE "." "_" ident ${base})
  set(spv ${OC_SPV_DIR}/${base}.spv)
  set(dxil ${OC_SPV_DIR}/${base}.dxil)
  set(msl ${OC_SPV_DIR}/${base}.msl)
  set(commands COMMAND ${OC_GLSLC} -O --target-env=vulkan1.0 ${ARGN} -o ${spv} ${OC_SHADER_SRC}/${src})
  set(embed_inputs -DSPV=${spv})
  if(OC_SHADERCROSS)
    list(APPEND commands
      COMMAND ${OC_SHADERCROSS} ${spv} -s SPIRV -d DXIL -o ${dxil}
      COMMAND ${OC_SHADERCROSS} ${spv} -s SPIRV -d MSL -o ${msl})
    list(APPEND embed_inputs -DDXIL=${dxil} -DMSL=${msl})
  endif()
  add_custom_command(
    OUTPUT ${OC_SPV_DIR}/${out}
    ${commands}
    COMMAND ${CMAKE_COMMAND} -DNAME=${ident} ${embed_inputs} -DOUT=${OC_SPV_DIR}/${out} -P ${CMAKE_CURRENT_SOURCE_DIR}/embed_shader.cmake
    DEPENDS ${OC_SHADER_SRC}/${src} ${CMAKE_CURRENT_SOURCE_DIR}/embed_shader.cmake
    COMMENT "shader ${src} -> ${out}"
    VERBATIM)
  set(OC_SHADER_INCS ${OC_SHADER_INCS} ${OC_SPV_DIR}/${out} PARENT_SCOPE)
endfunction()
foreach(sh sprite.vert sprite.frag mesh.vert mesh.frag particle.vert particle.frag screen.vert screen.frag stage.vert stage.frag shadow_cast.vert shadow_cast.frag shadow_floor.vert shadow_floor.frag shadow_blur.vert shadow_blur.frag post_fx.frag)
  oc_shader(${sh}.inc ${sh})
endforeach()
oc_shader(stage_affine.vert.inc stage.vert -DAFFINE)
oc_shader(stage_affine.frag.inc stage.frag -DAFFINE)

add_library(oc_render STATIC
  ${OC_SHADER_INCS}
  gpu_context.h
  gpu_context.cpp
  shaders.h
  shaders.cpp
  pipeline.h
  pipeline.cpp
  buffer.h
  buffer.cpp
  texture.h
  texture.cpp
  texture_registry.h
  texture_registry.cpp
  stage_tiles.h
  stage_tiles.cpp
  model_tiles.h
  model_tiles.cpp
  render_target.h
  render_target.cpp
  sprite_batch.h
  sprite_batch.cpp
  text_batch.h
  text_batch.cpp
  mat4.h
  camera.h
  mesh_pipeline.h
  mesh_pipeline.cpp
  stage_pipeline.h
  stage_pipeline.cpp
  shadow_pipeline.h
  shadow_pipeline.cpp
  particles.h
  particles.cpp
  screen_pass.h
  screen_pass.cpp
  post_fx.h
  post_fx.cpp
  ps1_jitter.h
  imgui_layer.h
  imgui_layer.cpp
  imgui_nav.h
  imgui_nav.cpp
  imgui_mods_tab.h
  imgui_mods_tab.cpp
  imgui_update_tab.h
  imgui_update_tab.cpp
  imgui_multiplayer.h
  imgui_multiplayer.cpp
  imgui_guide.h
  imgui_guide.cpp
)
target_include_directories(oc_render PRIVATE ${Stb_INCLUDE_DIR} ${OC_SPV_DIR})
target_link_libraries(oc_render PUBLIC
  oc_core
  oc_platform
  oc_net
  oc_vfs
  oc_text
  SDL3::SDL3
  imgui::imgui)
target_link_libraries(oc_render PRIVATE $<IF:$<TARGET_EXISTS:spng::spng>,spng::spng,spng::spng_static>)
target_compile_definitions(oc_render PRIVATE OC_SHADER_FORMATS=${OC_SHADER_FORMATS})
