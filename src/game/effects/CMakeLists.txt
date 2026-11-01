# Generic effect player: emitter system (0x0A), sequence player (0x10), CPU reference rasteriser and the battle slot table.
# No GPU dependency (the GPU glue is oc_effects_gpu below).
add_library(oc_effects STATIC
  fx_math.h
  fx_math.cpp
  strip_smooth.h
  strip_smooth.cpp
  light_extract.h
  light_extract.cpp
  emitter_system.h
  emitter_internal.h
  emitter_system.cpp
  emitter_spawn.cpp
  emitter_tick.cpp
  emitter_collect.cpp
  emitter_api.cpp
  sequence_player.h
  sequence_player.cpp
  soft_raster.h
  soft_raster.cpp
  effect_slots.h
  effect_slots.cpp
  bespoke/bespoke.h
  bespoke/bespoke_util.h
  bespoke/mesh_base.h
  bespoke/bespoke.cpp
  bespoke/band_wipe.cpp
  bespoke/line_ring.cpp
  bespoke/mesh_sweep.cpp
  bespoke/mesh_flight.cpp
  bespoke/mesh_water.cpp
  bespoke/motion_tasks.cpp
  bespoke/actor_script.h
  bespoke/actor_script.cpp
  bespoke/mesh_warp.cpp
  bespoke/scene_remap.cpp
  bespoke/overlay_base.h
  bespoke/model_steer.cpp
  bespoke/emitter_patch.cpp
  bespoke/model_draw.h
  bespoke/mesh_tech.h
  bespoke/model_deform.cpp
  bespoke/mesh_techs.cpp
  bespoke/death_ports.cpp
  bespoke/screen_ports.cpp
  bespoke/env_mesh.cpp
  bespoke/kamei_port.cpp
  bespoke/summon_ports.cpp
  bespoke/ground_ports.cpp
  bespoke/ground_ports_b.cpp
  bespoke/ground_ports_c.cpp
  bespoke/ground_base.h
  bespoke/tech_ports.cpp
)
target_link_libraries(oc_effects PUBLIC oc_core oc_platform oc_assets oc_import oc_audio oc_stage)  # oc_stage: the ground overlays parse pack meshes packet by packet

# GPU glue: sprite sheets as GPU textures + draws the sorted quads through render::ParticleBatch.
add_library(oc_effects_gpu STATIC
  gpu_effects.h
  gpu_effects.cpp
  room_meshes.h
  room_meshes.cpp
)
target_link_libraries(oc_effects_gpu PUBLIC oc_effects oc_render)
