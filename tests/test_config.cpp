// Config ini tests: the per-art-mode texture filter keys migrate to one global [Video] Filter.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "platform/config.h"

using namespace oc::platform;

namespace {
int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

Config load_text(const char* text) {
  const std::filesystem::path path = std::filesystem::temp_directory_path() / "oc_test_config.ini";
  { std::ofstream(path) << text; }
  Config config;
  CHECK(config.load(path));
  std::filesystem::remove(path);
  return config;
}
}  // namespace

int main() {
  // Old ini: FilterHD wins, the other per-art keys are ignored.
  Config legacy_hd = load_text("[Video]\nFilterHD=nearest\nFilterSD=bilinear\nFilterModded=bilinear\nArtMode=hd\nPs1Res3D=1\n");
  CHECK(legacy_hd.filter == TexFilter::Nearest);
  // Ps1Res3D never acted outside classic art: an old non-classic ini means off.
  CHECK(!legacy_hd.ps1_res);
  // Old ini in classic art keeps Ps1Res3D (migrated into Ps1Res).
  Config legacy_classic = load_text("[Video]\nFilterHD=bilinear\nArtMode=original\nPs1Res3D=1\n");
  CHECK(legacy_classic.filter == TexFilter::Bilinear);
  CHECK(legacy_classic.ps1_res);
  // `classic` is an alias of `original`; unknown values read as hd.
  CHECK(load_text("[Video]\nArtMode=classic\n").art_mode == ArtMode::Original);
  CHECK(load_text("[Video]\nArtMode=original\n").art_mode == ArtMode::Original);
  CHECK(load_text("[Video]\nArtMode=bogus\n").art_mode == ArtMode::Hd);
  // New key wins and is written; old keys are dropped on save.
  Config new_key = load_text("[Video]\nFilter=nearest\nFilterHD=bilinear\nPs1Res3D=1\nArtMode=modded\n");
  CHECK(new_key.filter == TexFilter::Nearest);
  CHECK(new_key.ps1_res);
  // The new key wins.
  CHECK(load_text("[Video]\nPs1Res=0\nPs1Res3D=1\n").ps1_res == false);
  const std::filesystem::path path = std::filesystem::temp_directory_path() / "oc_test_config_out.ini";
  CHECK(new_key.save(path));
  std::stringstream ss;
  { std::ifstream in(path); ss << in.rdbuf(); }
  std::filesystem::remove(path);
  const std::string saved = ss.str();
  CHECK(saved.find("Filter=nearest") != std::string::npos);
  CHECK(saved.find("FilterHD") == std::string::npos && saved.find("FilterSD") == std::string::npos && saved.find("FilterModded") == std::string::npos);
  Config defaults;
  CHECK(defaults.filter == TexFilter::Bilinear);
  CHECK(defaults.hd_battle_textures && !load_text("[Video]\nHdBattleTextures=0\n").hd_battle_textures);
  CHECK(defaults.smooth_water_line);
  CHECK(load_text("[Enhancements]\nSmoothWaterLine=1\n").smooth_water_line);
  // EdgeBlend defaults off.
  CHECK(!load_text("").edge_blend && load_text("[Enhancements]\nEdgeBlend=1\n").edge_blend);
  // The old SeamBlend level key maps to EdgeBlend off / on / on.
  CHECK(!load_text("[Enhancements]\nSeamBlend=0\n").edge_blend && load_text("[Enhancements]\nSeamBlend=1\n").edge_blend && load_text("[Enhancements]\nSeamBlend=2\n").edge_blend);
  CHECK(defaults.smooth_effect_rings && !defaults.lighting);
  // SmoothReflections defaults on; 0 = the PS1 texel steps.
  CHECK(defaults.smooth_reflections && !load_text("[Enhancements]\nSmoothReflections=0\n").smooth_reflections);
  CHECK(defaults.filter_effects == TexFilter::Bilinear && defaults.wide_edge_fade == 4 && !defaults.ps1_res);
  // The retired Toasts key is ignored.
  CHECK(load_text("[Video]\nToasts=0\n").wide_edge_fade == 4);
  Config rings = load_text("[Enhancements]\nSmoothEffectRings=1\n");
  CHECK(rings.smooth_effect_rings);
  const std::filesystem::path f2 = std::filesystem::temp_directory_path() / "oc_test_config_out2.ini";
  CHECK(rings.save(f2));
  std::stringstream ss2;
  { std::ifstream in(f2); ss2 << in.rdbuf(); }
  std::filesystem::remove(f2);
  CHECK(ss2.str().find("SmoothEffectRings=1") != std::string::npos);
  // FullPrecisionEffects defaults off and is written back.
  CHECK(!defaults.full_precision_effects);
  Config full_precision = load_text("[Enhancements]\nFullPrecisionEffects=1\n");
  CHECK(full_precision.full_precision_effects);
  const std::filesystem::path f4 = std::filesystem::temp_directory_path() / "oc_test_config_out4.ini";
  CHECK(full_precision.save(f4));
  std::stringstream ss4;
  { std::ifstream in(f4); ss4 << in.rdbuf(); }
  std::filesystem::remove(f4);
  CHECK(ss4.str().find("FullPrecisionEffects=1") != std::string::npos);
  // [Shaders]: all off by default; the styles, switches and full-screen options parse and are written back; the old [Video] Crt reads as crt.
  {
    CHECK(defaults.character_shader == CharacterShader::Off && defaults.matcap == "clay" && !defaults.outlines && !defaults.rim);
    CHECK(!defaults.spell_rim && !defaults.cast_glow && defaults.fullscreen_shader == FullscreenShader::Off && defaults.grade == "off");
    Config shaders = load_text("[Shaders]\nCharacters=matcap\nMatcap=chrome\nOutlines=1\nRim=1\nSpellRim=1\nCastGlow=1\n"
                               "Fullscreen=composite\nCrtMask=shadow\nCrtCurvature=1\nCrtGlow=1\nGrade=dusk\n");
    CHECK(shaders.character_shader == CharacterShader::Matcap && shaders.matcap == "chrome" && shaders.outlines && shaders.rim);
    CHECK(shaders.spell_rim && shaders.cast_glow);
    CHECK(shaders.fullscreen_shader == FullscreenShader::Composite && shaders.crt_mask == CrtMask::ShadowMask && shaders.crt_curvature && shaders.crt_glow);
    CHECK(shaders.grade == "dusk");
    CHECK(load_text("[Shaders]\nCharacters=cel\n").character_shader == CharacterShader::Cel);
    CHECK(load_text("[Shaders]\nCharacters=shiny\n").character_shader == CharacterShader::Off);
    CHECK(load_text("[Shaders]\nFullscreen=watercolour\n").fullscreen_shader == FullscreenShader::Off);
    CHECK(load_text("[Video]\nCrt=1\n").fullscreen_shader == FullscreenShader::Crt);
    CHECK(load_text("[Video]\nCrt=1\n[Shaders]\nFullscreen=off\n").fullscreen_shader == FullscreenShader::Off);
    const std::filesystem::path out = std::filesystem::temp_directory_path() / "oc_test_config_shaders.ini";
    CHECK(shaders.save(out));
    std::stringstream text;
    { std::ifstream in(out); text << in.rdbuf(); }
    Config reloaded;
    CHECK(reloaded.load(out));
    std::filesystem::remove(out);
    CHECK(text.str().find("Characters=matcap") != std::string::npos && text.str().find("Fullscreen=composite") != std::string::npos);
    CHECK(text.str().find("Crt=") == std::string::npos);
    CHECK(reloaded.character_shader == CharacterShader::Matcap && reloaded.matcap == "chrome" && reloaded.cast_glow && reloaded.grade == "dusk");
    CHECK(reloaded.fullscreen_shader == FullscreenShader::Composite && reloaded.crt_mask == CrtMask::ShadowMask);
  }
  // [Mods] dump categories: defaults, parse, save, reload (the Mods tab's "Dump all textures" checkboxes).
  {
    Config mods_defaults;
    CHECK(mods_defaults.dump_stages && mods_defaults.dump_title && mods_defaults.dump_rooms && mods_defaults.dump_models && !mods_defaults.dump_effects);
    Config mods_flipped = load_text("[Mods]\nDumpStages=0\nDumpTitle=0\nDumpRooms=0\nDumpModels=0\nDumpEffects=1\n");
    CHECK(!mods_flipped.dump_stages && !mods_flipped.dump_title && !mods_flipped.dump_rooms && !mods_flipped.dump_models && mods_flipped.dump_effects);
    const std::filesystem::path f3 = std::filesystem::temp_directory_path() / "oc_test_config_out3.ini";
    CHECK(mods_flipped.save(f3));
    std::stringstream ss3;
    { std::ifstream in(f3); ss3 << in.rdbuf(); }
    for (const char* key : {"DumpStages=0", "DumpTitle=0", "DumpRooms=0", "DumpModels=0", "DumpEffects=1"}) CHECK(ss3.str().find(key) != std::string::npos);
    Config mods_reloaded;
    CHECK(mods_reloaded.load(f3));
    std::filesystem::remove(f3);
    CHECK(!mods_reloaded.dump_stages && !mods_reloaded.dump_title && !mods_reloaded.dump_rooms && !mods_reloaded.dump_models && mods_reloaded.dump_effects);
    // The opposite way round: unlisted keys keep their defaults.
    Config mods_partial = load_text("[Mods]\nDumpStages=1\nDumpEffects=0\n");
    CHECK(mods_partial.dump_stages && mods_partial.dump_title && mods_partial.dump_rooms && mods_partial.dump_models && !mods_partial.dump_effects);
  }
  if (failures) return 1;
  std::printf("test_config: ok\n");
  return 0;
}
