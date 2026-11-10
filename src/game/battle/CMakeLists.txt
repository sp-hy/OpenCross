# Battle presentation: stage + model rendering, HUD and command menu over the headless battle core.
# Pure logic of the presentation layer (animation-sequence VM, camera shots / timelines): no rendering dependencies.
add_library(oc_battle_anim STATIC
  battle_seq.h
  battle_seq.cpp
  battle_camera.h
  battle_camera.cpp
  battle_numbers.h
  battle_numbers.cpp
)
target_link_libraries(oc_battle_anim PUBLIC oc_core oc_import oc_assets)

# Element / tech effects: effect-pack cache and the per-cast run (headless; no GPU).
add_library(oc_battle_fx STATIC
  battle_fx.h
  battle_fx.cpp
)
target_link_libraries(oc_battle_fx PUBLIC oc_battle_anim oc_stage oc_effects oc_battle oc_data oc_vfs oc_import oc_assets oc_core)

add_library(oc_battle_view STATIC
  battle_view.h
  battle_view_op.h
  battle_model_cache.h
  battle_model_cache.cpp
  battle_spell_lights.h
  battle_spell_lights.cpp
  stage_key_light.h
  stage_key_light.cpp
  battle_fx_textures.h
  battle_fx_textures.cpp
  battle_audit.h
  battle_view_fx.cpp
  battle_view_fx_cast.cpp
  battle_view_fx_state.cpp
  battle_view_fx_lights.cpp
  battle_view_fx_skin.cpp
  battle_view_fx_draw.cpp
  battle_transition.cpp
  battle_view.cpp
  battle_view_action.cpp
  battle_view_script.cpp
  battle_view_units.cpp
  battle_view_sequence.cpp
  battle_view_anchor.cpp
  battle_view_anm.cpp
  battle_view_camera.cpp
  battle_view_hud.cpp
  battle_view_draw.cpp
  battle_view_shadows.cpp
  battle_view_trail.cpp
  battle_view_pseudo.cpp
  battle_view_showcase.cpp
  battle_view_ui.cpp
  battle_view_versus.cpp
  texture_dump.h
  texture_dump.cpp
  battle_pose.h
  battle_pose.cpp
  battle_audio.h
  battle_audio.cpp
  stage_scene.h
  stage_scene.cpp
  battle_results.h
  battle_results.cpp
)
target_link_libraries(oc_battle_view PUBLIC oc_battle_fx oc_effects_gpu oc_battle_anim oc_stage oc_text oc_ui oc_game oc_battle oc_versus_link oc_data oc_render oc_audio oc_import oc_vfs oc_platform oc_core SDL3::SDL3)


# The versus links: the local opponent and the other game over the multiplayer client
add_library(oc_versus_link STATIC
  versus_link.h
  versus_link.cpp
  versus_net_link.h
  versus_net_link.cpp
)
target_link_libraries(oc_versus_link PUBLIC oc_battle oc_data oc_net oc_core)
