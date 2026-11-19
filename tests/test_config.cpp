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

Config load_text(const char* text, bool player_ini = false) {
  const std::filesystem::path path = std::filesystem::temp_directory_path() / "oc_test_config.ini";
  { std::ofstream(path) << text; }
  Config config;
  if (player_ini) config.use_player_defaults();
  CHECK(config.load(path));
  std::filesystem::remove(path);
  return config;
}
}  // namespace

int main() {
  // Old ini: FilterHD wins, the other per-art keys are ignored.
  Config legacy_hd = load_text("[Video]\nFilterHD=nearest\nFilterSD=bilinear\nFilterModded=bilinear\nArtMode=hd\nPs1Res3D=1\n");
  CHECK(legacy_hd.filter == TexFilter::Nearest);
  // A UTF-8 byte order mark before the first section does not hide it.
  CHECK(load_text("\xEF\xBB\xBF[Video]\nFilterHD=nearest\n").filter == TexFilter::Nearest);
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
  // EdgeBlend defaults off.
  CHECK(!load_text("").edge_blend && load_text("[Enhancements]\nEdgeBlend=1\n").edge_blend);
  // The old SeamBlend level key maps to EdgeBlend off / on / on.
  CHECK(!load_text("[Enhancements]\nSeamBlend=0\n").edge_blend && load_text("[Enhancements]\nSeamBlend=1\n").edge_blend && load_text("[Enhancements]\nSeamBlend=2\n").edge_blend);
  CHECK(!defaults.lighting);
  CHECK(defaults.filter_effects == TexFilter::Bilinear && defaults.wide_edge_fade == 4 && !defaults.ps1_res_graphics && !defaults.ps1_res_ui);
  // The retired Toasts key is ignored.
  CHECK(load_text("[Video]\nToasts=0\n").wide_edge_fade == 4);
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
  // LegacyFieldModels: on in a --config ini, off in the player's ini, whose four Graphics enhancements default on; stored keys win.
  CHECK(defaults.legacy_field_models && !defaults.retain_battle_scenery);
  {
    const Config player = load_text("", true);
    CHECK(!player.legacy_field_models && player.lighting && player.edge_blend && player.retain_battle_scenery && player.full_precision_effects);
    CHECK(!player.autohide_cast_hud && !player.hide_feature_icons && !player.equip_compare && !defaults.equip_compare);
    CHECK(load_text("[Enhancements]\nEquipCompare=1\n", true).equip_compare);
    CHECK(!player.music_resume && !player.auto_advance && player.auto_advance_delay == 100);
    const Config qol = load_text("[Game]\nAutoAdvance=1\nAutoAdvanceDelay=900\n[Audio]\nMusicResume=1\n");
    CHECK(qol.auto_advance && qol.auto_advance_delay == 300 && qol.music_resume);
    // OpenCrossMenu: on in the player's ini, off in a --config one; the stored value wins.
    CHECK(player.opencross_menu && !defaults.opencross_menu && load_text("[Game]\nOpenCrossMenu=1\n").opencross_menu);
    CHECK(!load_text("[Game]\nOpenCrossMenu=0\n", true).opencross_menu);
    CHECK(player.achievements && player.hide_achievement_notices && !defaults.achievements && !defaults.hide_achievement_notices);
    CHECK(!load_text("[Game]\nHideAchievementNotices=0\n", true).hide_achievement_notices);
    CHECK(player.render_scale == 2.0f && defaults.render_scale == 1.0f && load_text("[Video]\nRenderScale=1\n", true).render_scale == 1.0f);
    const Config stored = load_text("[Video]\nLegacyFieldModels=1\n[Enhancements]\nLighting=0\nEdgeBlend=0\n", true);
    CHECK(stored.legacy_field_models && !stored.lighting && !stored.edge_blend && stored.full_precision_effects);
    CHECK(!load_text("[Enhancements]\nMenuModelsInField=0\n", true).legacy_field_models);
    CHECK(!load_text("[Video]\nLegacyFieldModels=0\n").legacy_field_models);
  }
  // [Shaders]: all off by default; the styles, switches and full-screen options parse and are written back; the old [Video] Crt reads as crt.
  {
    CHECK(defaults.character_shader == CharacterShader::Off && defaults.matcap == "clay" && !defaults.outlines && !defaults.rim);
    CHECK(!defaults.spell_rim && !defaults.cast_glow && defaults.fullscreen_shader == FullscreenShader::Off && defaults.grade == "off");
    Config shaders = load_text("[Shaders]\nCharacters=matcap\nMatcap=chrome\nOutlines=1\nRim=1\nSpellRim=1\nCastGlow=1\n"
                               "Fullscreen=composite\nCrtMask=shadow\nCrtCurvature=1\nCrtGlow=1\nGrade=dusk\nSsao=1\nDepthOfField=1\n");
    CHECK(!defaults.ssao && !defaults.depth_of_field && shaders.ssao && shaders.depth_of_field);
    CHECK(load_text("[Shaders]\nSsao=1\n").ssao && !load_text("[Shaders]\nSsao=1\n").depth_of_field);
    CHECK(shaders.character_shader == CharacterShader::Matcap && shaders.matcap == "chrome" && shaders.outlines && shaders.rim);
    CHECK(shaders.spell_rim && shaders.cast_glow);
    CHECK(shaders.fullscreen_shader == FullscreenShader::Composite && shaders.crt_mask == CrtMask::ShadowMask && shaders.crt_curvature && shaders.crt_glow);
    CHECK(shaders.grade == "dusk");
    CHECK(load_text("[Shaders]\nCharacters=cel\n").character_shader == CharacterShader::Cel);
    CHECK(load_text("[Shaders]\nCharacters=shiny\n").character_shader == CharacterShader::Off);
    CHECK(load_text("[Shaders]\nFullscreen=watercolour\n").fullscreen_shader == FullscreenShader::Off);
    CHECK(load_text("[Video]\nCrt=1\n").fullscreen_shader == FullscreenShader::Crt);
    CHECK(load_text("[Video]\nCrt=1\n[Shaders]\nFullscreen=off\n").fullscreen_shader == FullscreenShader::Off);
    // The strengths and the outline width: full by default, clamped, written back.
    CHECK(defaults.style_strength == 1.0f && defaults.rim_strength == 1.0f && defaults.outline_width == 1.0f && defaults.outline_depth);
    const Config tuned = load_text("[Shaders]\nStyleStrength=0.25\nRimStrength=1.5\nOutlineWidth=9\nOutlineDepth=0\n");
    CHECK(tuned.style_strength == 0.25f && tuned.rim_strength == 1.0f && tuned.outline_width == 4.0f && !tuned.outline_depth);
    CHECK(load_text("[Shaders]\nRimStrength=-1\n").rim_strength == 0.0f);
    CHECK(load_text("[Shaders]\nOutlineWidth=0.5\n").outline_width == 0.5f);
    CHECK(defaults.ssao_range == 2.0f && defaults.dof_focus_range == 1.0f && defaults.dof_strength == 1.0f);
    const Config post = load_text("[Shaders]\nSsaoRange=40\nDofFocusRange=0.1\nDofStrength=1.5\n");
    CHECK(post.ssao_range == 10.0f && post.dof_focus_range == 0.25f && post.dof_strength == 1.5f);
    CHECK(load_text("[Shaders]\nSsaoRange=0\n").ssao_range == 0.0f);
    shaders.ssao_range = 3.5f;
    shaders.dof_strength = 0.5f;
    shaders.style_strength = 0.5f;
    shaders.outline_width = 1.75f;
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
    CHECK(reloaded.ssao && reloaded.depth_of_field && text.str().find("DepthOfField=1") != std::string::npos);
    CHECK(reloaded.style_strength == 0.5f && reloaded.outline_width == 1.75f && reloaded.rim_strength == 1.0f && reloaded.outline_depth);
    CHECK(reloaded.ssao_range == 3.5f && reloaded.dof_strength == 0.5f && reloaded.dof_focus_range == 1.0f);
  }
  // [Mods] dump categories: defaults, parse, save, reload (the Mods tab's "Dump all textures" checkboxes).
  {
    Config mods_defaults;
    CHECK(!mods_defaults.dump_model_source && load_text("[Mods]\nDumpModelSource=1\n").dump_model_source);
    CHECK(load_text("[Mods]\nDumpModelSource=1\n").mods.empty());
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
  // [Guide]: off with no guide by default; both keys survive a save and reload.
  {
    const Config guide_defaults;
    CHECK(!guide_defaults.guide_enabled && guide_defaults.guide_url.empty());
    Config guide = load_text("[Guide]\nEnabled=1\nUrl=https://gamefaqs.gamespot.com/ps/196917-chrono-cross/faqs/48347\n");
    CHECK(guide.guide_enabled && guide.guide_url == "https://gamefaqs.gamespot.com/ps/196917-chrono-cross/faqs/48347");
    const std::filesystem::path out = std::filesystem::temp_directory_path() / "oc_test_config_guide.ini";
    CHECK(guide.save(out));
    Config reloaded;
    CHECK(reloaded.load(out));
    std::filesystem::remove(out);
    CHECK(reloaded.guide_enabled && reloaded.guide_url == guide.guide_url);
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
  // Data source: auto with no discs and no region by default; the words parse, unknown ones keep the default, all survive a save.
  {
    const Config source_defaults;
    CHECK(source_defaults.source == DataSource::Auto && source_defaults.disc1.empty() && source_defaults.disc2.empty());
    CHECK(source_defaults.disc_dir.empty() && source_defaults.disc_region.empty());
    CHECK(load_text("[Paths]\nSource=rde\n").source == DataSource::Rde);
    CHECK(load_text("[Paths]\nSource=disc\n").source == DataSource::Disc);
    CHECK(load_text("[Paths]\nSource=Disc\n").source == DataSource::Auto);
    CHECK(load_text("[Disc]\nRegion=usa\n").disc_region == "usa");
    CHECK(load_text("[Disc]\nRegion=jp\n").disc_region == "jp");
    CHECK(load_text("[Disc]\nRegion=pal\n").disc_region.empty());
    Config discs = load_text("[Paths]\nSource=disc\nDisc1=D:\\Games\\Chrono Cross (Disc 1).cue\nDisc2=D:\\Games\\Chrono Cross (Disc 2).chd\n"
                             "DiscDir=..\\images\n[Disc]\nRegion=usa\n");
    CHECK(discs.disc1 == "D:\\Games\\Chrono Cross (Disc 1).cue" && discs.disc2 == "D:\\Games\\Chrono Cross (Disc 2).chd");
    CHECK(discs.disc_dir == "..\\images");
    const std::filesystem::path out = std::filesystem::temp_directory_path() / "oc_test_config_disc.ini";
    CHECK(discs.save(out));
    Config reloaded;
    CHECK(reloaded.load(out));
    std::stringstream text;
    text << std::ifstream(out, std::ios::binary).rdbuf();
    std::filesystem::remove(out);
    CHECK(reloaded.source == DataSource::Disc && reloaded.disc1 == discs.disc1 && reloaded.disc2 == discs.disc2);
    CHECK(reloaded.disc_dir == discs.disc_dir && reloaded.disc_region == "usa");
    CHECK(text.str().find("[Disc]\nRegion=usa\n") != std::string::npos);
    const std::filesystem::path blank = std::filesystem::temp_directory_path() / "oc_test_config_disc_blank.ini";
    CHECK(source_defaults.save(blank));
    std::stringstream blank_text;
    blank_text << std::ifstream(blank, std::ios::binary).rdbuf();
    std::filesystem::remove(blank);
    CHECK(blank_text.str().find("Source=auto\nDisc1=\nDisc2=\nDiscDir=\n") != std::string::npos);
  }
  if (failures) return 1;
  std::printf("test_config: ok\n");
  return 0;
}
