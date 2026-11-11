// Config ini tests: the per-art-mode texture filter keys migrate to one global [Video] Filter.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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
  CHECK(!legacy_hd.ps1_res_graphics && !legacy_hd.ps1_res_ui);
  // Old ini in classic art keeps Ps1Res3D (migrated through Ps1Res into both halves).
  Config legacy_classic = load_text("[Video]\nFilterHD=bilinear\nArtMode=original\nPs1Res3D=1\n");
  CHECK(legacy_classic.filter == TexFilter::Bilinear);
  CHECK(legacy_classic.ps1_res_graphics && legacy_classic.ps1_res_ui);
  // `classic` is an alias of `original`; unknown values read as hd.
  CHECK(load_text("[Video]\nArtMode=classic\n").art_mode == ArtMode::Original);
  CHECK(load_text("[Video]\nArtMode=original\n").art_mode == ArtMode::Original);
  CHECK(load_text("[Video]\nArtMode=bogus\n").art_mode == ArtMode::Hd);
  // New key wins and is written; old keys are dropped on save.
  Config new_key = load_text("[Video]\nFilter=nearest\nFilterHD=bilinear\nPs1Res3D=1\nArtMode=modded\n");
  CHECK(new_key.filter == TexFilter::Nearest);
  CHECK(new_key.ps1_res_graphics && new_key.ps1_res_ui);
  // The newer key wins.
  CHECK(!load_text("[Video]\nPs1Res=0\nPs1Res3D=1\n").ps1_res_graphics);
  // Ps1Res is the default of both halves; each split key wins on its own.
  const Config split = load_text("[Video]\nPs1Res=1\nPs1ResUi=0\n");
  CHECK(split.ps1_res_graphics && !split.ps1_res_ui);
  const Config ui_only = load_text("[Video]\nPs1ResGraphics=0\nPs1ResUi=1\n");
  CHECK(!ui_only.ps1_res_graphics && ui_only.ps1_res_ui);
  const std::filesystem::path path = std::filesystem::temp_directory_path() / "oc_test_config_out.ini";
  CHECK(new_key.save(path));
  std::stringstream ss;
  { std::ifstream in(path); ss << in.rdbuf(); }
  std::filesystem::remove(path);
  const std::string saved = ss.str();
  CHECK(saved.find("Filter=nearest") != std::string::npos);
  CHECK(saved.find("Ps1ResGraphics=1") != std::string::npos && saved.find("Ps1ResUi=1") != std::string::npos && saved.find("Ps1Res=") == std::string::npos);
  CHECK(saved.find("FilterHD") == std::string::npos && saved.find("FilterSD") == std::string::npos && saved.find("FilterModded") == std::string::npos);
  Config defaults;
  CHECK(defaults.filter == TexFilter::Bilinear);
  CHECK(defaults.hd_battle_textures && !load_text("[Video]\nHdBattleTextures=0\n").hd_battle_textures);
  CHECK(defaults.smooth_water_line);
  CHECK(load_text("[Enhancements]\nSmoothWaterLine=1\n").smooth_water_line);
  // ModelPixelDepth defaults on; 0 = the PS1's per-face order.
  CHECK(defaults.model_pixel_depth && !load_text("[Enhancements]\nModelPixelDepth=0\n").model_pixel_depth);
  // EdgeBlend defaults off.
  CHECK(!load_text("").edge_blend && load_text("[Enhancements]\nEdgeBlend=1\n").edge_blend);
  // The old SeamBlend level key maps to EdgeBlend off / on / on.
  CHECK(!load_text("[Enhancements]\nSeamBlend=0\n").edge_blend && load_text("[Enhancements]\nSeamBlend=1\n").edge_blend && load_text("[Enhancements]\nSeamBlend=2\n").edge_blend);
  CHECK(defaults.smooth_effect_rings && !defaults.lighting);
  // SmoothReflections defaults on; 0 = the PS1 texel steps.
  CHECK(defaults.smooth_reflections && !load_text("[Enhancements]\nSmoothReflections=0\n").smooth_reflections);
  CHECK(defaults.filter_effects == TexFilter::Bilinear && defaults.wide_edge_fade == 4 && !defaults.ps1_res_graphics && !defaults.ps1_res_ui);
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
  // [Multiplayer]: defaults (off, the master server), parse with clamps and unknown words, save, reload.
  {
    const MultiplayerSettings mp_defaults;
    CHECK(!mp_defaults.enabled && mp_defaults.server == MultiplayerServer::Master && mp_defaults.url() == kMasterUrl);
    CHECK(mp_defaults.server_url == "ws://127.0.0.1:8787/ws" && mp_defaults.max_parties == 8 && mp_defaults.show_players && mp_defaults.show_labels && mp_defaults.chat_bubbles && !mp_defaults.hide_global_chat);
    CHECK(!mp_defaults.translucent && mp_defaults.chat == ChatDisplay::New && mp_defaults.muted.empty() && mp_defaults.admin_token.empty());
    CHECK(mp_defaults.challenges && !load_text("[Multiplayer]\nChallenges=0\n").multiplayer.challenges);
    CHECK(load_text("[Multiplayer]\nAdminToken=s3cret\n").multiplayer.admin_token == "s3cret");
    Config mp = load_text("[Multiplayer]\nEnabled=1\nServer=other\nServerUrl=wss://example.invalid/ws\nName=Kid\n"
                          "PlayerId=0123456789abcdef0123456789abcdef\nShowPlayers=0\nMaxParties=99\nTranslucent=1\nShowLabels=0\nChatBubbles=0\nHideGlobalChat=1\n"
                          "Chat=always\nMuteHidesParty=0\nChallenges=0\nMuted= aa11 ,bb22,,cc33\n");
    const MultiplayerSettings& parsed = mp.multiplayer;
    CHECK(parsed.enabled && parsed.server == MultiplayerServer::Other && parsed.url() == "wss://example.invalid/ws");
    CHECK(parsed.name == "Kid" && parsed.player_id == "0123456789abcdef0123456789abcdef" && !parsed.show_players && parsed.max_parties == 32);
    CHECK(parsed.translucent && !parsed.show_labels && !parsed.chat_bubbles && parsed.hide_global_chat && parsed.chat == ChatDisplay::Always && !parsed.mute_hides_party);
    CHECK(parsed.muted == std::vector<std::string>({"aa11", "bb22", "cc33"}));
    CHECK(load_text("[Multiplayer]\nServer=moon\nChat=loud\nMaxParties=0\n").multiplayer.server == MultiplayerServer::Master);
    CHECK(load_text("[Multiplayer]\nChat=loud\n").multiplayer.chat == ChatDisplay::New);
    CHECK(load_text("[Multiplayer]\nMaxParties=0\n").multiplayer.max_parties == 1);
    const std::filesystem::path out = std::filesystem::temp_directory_path() / "oc_test_config_mp.ini";
    CHECK(mp.save(out));
    Config reloaded;
    CHECK(reloaded.load(out));
    std::filesystem::remove(out);
    const MultiplayerSettings& again = reloaded.multiplayer;
    CHECK(again.enabled && again.server == MultiplayerServer::Other && again.server_url == parsed.server_url && again.name == "Kid");
    CHECK(again.player_id == parsed.player_id && !again.show_players && again.max_parties == 32 && again.translucent && !again.show_labels && !again.chat_bubbles && again.hide_global_chat);
    CHECK(again.chat == ChatDisplay::Always && !again.mute_hides_party && again.muted == parsed.muted && !parsed.challenges && !again.challenges);
  }
  if (failures) return 1;
  std::printf("test_config: ok\n");
  return 0;
}
