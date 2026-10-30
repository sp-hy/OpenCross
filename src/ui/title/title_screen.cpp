#include "ui/title/title_screen.h"

#include <algorithm>
#include <cmath>
#include <iterator>

#include "audio/bgm_table.h"
#include "core/log.h"
#include "import/tim.h"

namespace oc::ui {

namespace {

using render::pack_rgba;
constexpr int lang_index(import::Lang lang) { return static_cast<int>(lang); }

// Label tables (EN FR IT DE ES JP): variant 1, then variant 2 (New Game+; JP = 強くて + the English word, no space).
constexpr const char32_t* kNew[6] = {U"New Game", U"Nouvelle partie", U"Nuova partita", U"Neues Spiel", U"Nueva partida", U"New Game"};
constexpr const char32_t* kCont[6] = {U"Continue", U"Continuer", U"Continua", U"Spiel fortsetzen", U"Continuar", U"Continue"};
constexpr const char32_t* kNewPlus[6] = {U"New Game +", U"Nouvelle partie +", U"Nuova partita +", U"Neues Spiel +", U"Nueva partida +",
                                         U"強くてNew Game"};
constexpr const char32_t* kContPlus[6] = {U"Continue +", U"Continuer +", U"Continua +", U"Spiel fortsetzen +", U"Continuar +",
                                          U"強くてContinue"};
// The vibration page: On / Off and the footer "Vibration Function".
constexpr const char32_t* kOn[6] = {U"On", U"Oui", U"Sì", U"An", U"Sí", U"ON"};
constexpr const char32_t* kOff[6] = {U"Off", U"Non", U"No", U"Aus", U"No", U"OFF"};
constexpr const char32_t* kVibration[6] = {U"Vibration Function", U"Fonction de vibration", U"Funzione di vibrazione", U"Vibration",
                                           U"Función de vibración", U"バイブレーション機能"};
constexpr const char32_t* kSettings[6] = {U"Settings", U"Options", U"Opzioni", U"Optionen", U"Opciones", U"Settings"};
constexpr const char32_t* kQuit[6] = {U"Quit", U"Quitter", U"Esci", U"Beenden", U"Salir", U"Quit"};
constexpr const char32_t* kPrompt[6] = {U"Press any button", U"Appuyez sur une touche.", U"Premi un pulsante qualsiasi",
                                        U"Beliebige Taste drücken", U"Pulsa cualquier botón", U"Press any button"};

// Fade duration in seconds.
constexpr float kFade = 0.5f;
// The press: the title holds kPressFadeDelay frames (60 Hz), then a subtractive fade reaches black over kPressFadeFrames.
constexpr float kPressFadeDelay = 22.0f;
constexpr float kPressFadeFrames = 32.0f;

// The press-start chime: three global sounds started together, one per layer.
constexpr int kChimeSounds[3] = {59, 61, 62};
// After the press the music fades to silence: it starts kMusicFadeDelay frames (60 Hz) after the press, lasts kMusicFadeFrames
// frames and the song is stopped when it ends.
constexpr float kMusicFadeDelay = 14.0f;
constexpr float kMusicFadeFrames = 20.0f;

// Decodes a TIM into an uploaded RGBA image (nearest sampled).
render::TexRef upload_tim(UiKit& kit, const char* path, float* w, float* h) {
  auto raw = kit.vfs().read(path);
  if (!raw) return {};
  auto tim = import::parse_tim(*raw);
  if (!tim) return {};
  auto img = import::decode_tim(*tim);
  if (!img) return {};
  import::Image rgba = import::to_rgba(*img);
  if (w) *w = static_cast<float>(rgba.width);
  if (h) *h = static_cast<float>(rgba.height);
  return kit.upload_rgba(rgba.width, rgba.height, rgba.pixels.data(), false);
}

}  // namespace

bool TitleScreen::init(UiKit& kit, audio::Mixer* mixer) {
  kit_ = &kit;
  mixer_ = mixer;
  if (auto png = kit.vfs().read("hd/title/title.png")) logo_ = kit.upload_png(*png);
  if (!logo_) log::warn("title: hd/title/title.png unavailable");
  bg_[0] = upload_tim(kit, "cdrom/menu/title/bg0.tim", nullptr, nullptr);
  bg_[1] = upload_tim(kit, "cdrom/menu/title/bg1.tim", nullptr, nullptr);
  if (!bg_[0]) log::warn("title: cdrom/menu/title/bg0.tim unavailable");
  // Underline atlases: map/etc/openfont.tim (variant 1) and menu/title/cmptitle.tim (variant 2). Both are cdrom2-first in either art
  // mode (Vfs::cdrom2_first), i.e. the blanked copies: only the underline of each 32-row label cell is left.
  bars_[0] = upload_tim(kit, "cdrom/map/etc/openfont.tim", nullptr, &bars_h_[0]);
  bars_[1] = upload_tim(kit, "cdrom/menu/title/cmptitle.tim", nullptr, &bars_h_[1]);
  return true;
}

void TitleScreen::shutdown() {
  // The images are owned (and freed) by the UiKit.
  kit_ = nullptr;
  logo_ = {};
  bg_[0] = bg_[1] = {};
  bars_[0] = bars_[1] = {};
}

void TitleScreen::begin(import::Lang lang, ArtMode mode, bool has_saves, int song, bool backdrop) {
  lang_ = lang;
  mode_ = mode;
  has_saves_ = has_saves;
  backdrop_ = backdrop;
  variant_ = 1;
  page_ = 0;
  phase_ = backdrop ? Phase::Prompt : Phase::FadeIn;
  choice_ = pending_ = TitleChoice::None;
  sel_ = 0;
  fade_ = backdrop ? 0.0f : 1.0f;
  to_menu_ = menu_in_ = to_prompt_ = false;
  time_ = 0;
  music_frames_ = -1.0f;
  song_ = song;
  start_song(song);
}

void TitleScreen::begin_plus(import::Lang lang, ArtMode mode) {
  lang_ = lang;
  mode_ = mode;
  has_saves_ = true;
  backdrop_ = false;
  variant_ = 2;
  page_ = 0;
  // The list opens directly (no prompt); it fades in from the black ending screen.
  phase_ = Phase::Menu;
  menu_in_ = true;
  to_menu_ = to_prompt_ = false;
  choice_ = pending_ = TitleChoice::None;
  sel_ = 0;
  fade_ = 1.0f;
  time_ = 0;
  music_frames_ = -1.0f;
}

void TitleScreen::start_song(int song) {
  if (!mixer_ || song < 0 || !kit_) return;
  const audio::BgmEntry* entry = audio::bgm_entry(song);
  if (!entry) return;
  if (mixer_->current_song() == song) return;
  auto bytes = kit_->vfs().read(audio::bgm_path(entry->file_no));
  auto stream = std::make_shared<audio::HcaStream>();
  if (!bytes || !stream->open(*bytes)) {
    log::warn("title: could not load {}", audio::bgm_path(entry->file_no));
    return;
  }
  mixer_->play(std::move(stream), song, 127);
}

void TitleScreen::press() {
  to_menu_ = true;
  press_frames_ = 0.0f;
  if (layered_sfx_)
    for (int layer = 0; layer < static_cast<int>(std::size(kChimeSounds)); ++layer) layered_sfx_(kChimeSounds[layer], layer);
  music_frames_ = 0.0f;
}

void TitleScreen::step_music(float dt) {
  if (music_frames_ < 0.0f || !mixer_) return;
  const float before = music_frames_;
  music_frames_ += dt * 60.0f;
  if (before < kMusicFadeDelay && music_frames_ >= kMusicFadeDelay) mixer_->fade(0, kMusicFadeFrames / 60.0f);
  if (music_frames_ >= kMusicFadeDelay + kMusicFadeFrames) {
    mixer_->stop();
    music_frames_ = -1.0f;
  }
}

std::u32string TitleScreen::label(int i) const {
  const int lang_idx = std::min(lang_index(lang_), 5);
  if (page_ == 1) return i == 0 ? kOn[lang_idx] : kOff[lang_idx];
  if (variant_ == 2) return i == 0 ? kNewPlus[lang_idx] : kContPlus[lang_idx];
  switch (i) {
    case 0: return kNew[lang_idx];
    case 1: return kCont[lang_idx];
    case 2: return kSettings[lang_idx];
    default: return kQuit[lang_idx];
  }
}

void TitleScreen::update(const Nav& nav, float dt) {
  time_ += dt;
  step_music(dt);
  switch (phase_) {
    case Phase::FadeIn:
      fade_ = std::max(0.0f, fade_ - dt / kFade);
      if (fade_ <= 0) phase_ = Phase::Prompt;
      // Any button skips the fade-in and the prompt: straight to the fade-out into the menu.
      if (nav.any) {
        fade_ = 0;
        phase_ = Phase::Prompt;
        press();
      }
      break;
    case Phase::Prompt:
      // After a button press the title fades to black, then the menu fades in; after a cancel the title fades in again from black.
      if (to_menu_) {
        press_frames_ += dt * 60.0f;
        if (press_frames_ >= kPressFadeDelay) fade_ = std::min(1.0f, fade_ + dt * 60.0f / kPressFadeFrames);
        if (fade_ >= 1.0f) {
          to_menu_ = false;
          menu_in_ = true;
          phase_ = Phase::Menu;
        }
      } else if (fade_ > 0.0f) {
        fade_ = std::max(0.0f, fade_ - dt / kFade);
      } else if (nav.any) {
        press();
      }
      break;
    case Phase::Menu: {
      if (menu_in_) {
        fade_ = std::max(0.0f, fade_ - dt / kFade);
        if (fade_ <= 0.0f) menu_in_ = false;
      }
      // The vibration page: confirm stores the choice (On = first entry) and starts the game; Cross returns to page 0, cursor 0.
      if (page_ == 1) {
        if (menu_in_) break;
        if (nav.up || nav.down) sel_ ^= 1, play(1);
        if (nav.confirm) {
          play(8);
          vibration_ = sel_ == 0;
          pending_ = TitleChoice::NewGame;
          phase_ = Phase::FadeOut;
        } else if (nav.cancel) {
          play(6);
          page_ = 0;
          sel_ = 0;
        }
        break;
      }
      // Cancel goes back to the 3D title ("Press any button") through black like the way in; the title music starts again.
      if (nav.cancel && !menu_in_ && !to_prompt_ && variant_ != 2) {
        to_prompt_ = true;
        play(6);
      }
      if (to_prompt_) {
        fade_ = std::min(1.0f, fade_ + dt / kFade);
        if (fade_ >= 1.0f) {
          to_prompt_ = false;
          phase_ = Phase::Prompt;
          time_ = 0;
          start_song(song_);
        }
        break;
      }
      const int cnt = item_count();
      if (nav.up) sel_ = (sel_ + cnt - 1) % cnt, play(1);
      if (nav.down) sel_ = (sel_ + 1) % cnt, play(1);
      if (nav.confirm) {
        if (!enabled(sel_)) {
          play(0x18);
          break;
        }
        play(8);
        static constexpr TitleChoice kMap[4] = {TitleChoice::NewGame, TitleChoice::Continue, TitleChoice::Settings, TitleChoice::Quit};
        // New Game (+) opens the vibration page.
        if (kMap[sel_] == TitleChoice::NewGame) page_ = 1, sel_ = 0;
        else if (kMap[sel_] == TitleChoice::Settings) choice_ = TitleChoice::Settings;
        else pending_ = kMap[sel_], phase_ = Phase::FadeOut;
      }
      break;
    }
    case Phase::FadeOut:
      fade_ = std::min(1.0f, fade_ + dt / kFade);
      if (fade_ >= 1.0f) {
        choice_ = pending_;
        pending_ = TitleChoice::None;
      }
      break;
  }
}

void TitleScreen::draw(DrawList& dl) const {
  if (!kit_) return;
  const render::TexRef white = kit_->white();
  // HD art draws the splash, prompt and menu text itself; classic art shows the PS1 title room layers instead (logo, (c), prompt)
  // and only the menu text via small_font_*.
  kit_->set_classic(mode_ == ArtMode::Original);
  const bool menu_phase = phase_ == Phase::Menu || phase_ == Phase::FadeOut;
  // Classic art + [Video] Ps1ResMenus: the menu (bg0 / bg1 = one 640 x 448 screen) is drawn at 640 x 448. The splash / prompt
  // over the 3D title scene stay at window resolution.
  if (menu_phase) kit_->request_lowres(dl, kMenuWide);
  if (menu_phase || !backdrop_) {
    // Menu: the PS1 menu background bg0 (640x256) over bg1 (640x192), one 640x448 screen stretched to the canvas,
    // opaque over the title room. Same art in both modes.
    dl.rect(white, 0, 0, kCanvasW, kCanvasHeight, pack_rgba(0, 0, 0));
    if (!menu_phase) {
      dl.image(logo_, 0, 0, kCanvasW, kCanvasHeight);
    } else {
      const float ps1_scale = kCanvasHeight / 448.0f;
      dl.image(bg_[0], 0, 0, kCanvasW, 256 * ps1_scale);
      dl.image(bg_[1], 0, 256 * ps1_scale, kCanvasW, 192 * ps1_scale);
    }
  } else if (mode_ == ArtMode::Hd) {
    // Logo splash: hd/title/title.png (logo + copyright; RGBA, transparent outside the lettering) composited over the room scene,
    // rect x -7, y -25.6, 1286.4 x 1013.14. The PS1 logo / prompt / (c) layers are hidden (App::draw_frame, set_hidden_groups).
    dl.image(logo_, -7.0f, -25.6f, 1286.4f, 1013.14f);
  }

  if (phase_ == Phase::Prompt && mode_ == ArtMode::Original && backdrop_) {
    // Classic art over the 3D title room: the disc's own room layers draw the logo and prompt (the app leaves them visible).
  } else if (phase_ == Phase::Prompt) {
    // Prompt: slot 4 (bn_70) at 0.5 * 0.95, centred on 640, y 700.111; Normal 0.9, Outline = Shadow 0.13.
    // Blink alpha over a 70-frame cycle (60 fps): cnt 0..20 = cnt / 20 (fade in), 21..60 = (60 - cnt) / 40 (fade out), 61..69 hidden.
    // Normal and Outline = Shadow both carry it.
    const int cnt = static_cast<int>(std::fmod(time_ * 60.0f, 70.0f));
    const float a = cnt <= 20 ? static_cast<float>(cnt) / 20.0f : cnt <= 60 ? static_cast<float>(60 - cnt) / 40.0f : 0.0f;
    if (cnt <= 60 && a > 0.0f) {
      const std::u32string prompt = kPrompt[std::min(lang_index(lang_), 5)];
      const u8 ab = static_cast<u8>(a * 255.0f + 0.5f);
      const font::Ink ink{pack_rgba(230, 230, 230, ab), pack_rgba(33, 33, 33, ab), true, false};
      dl.text(kit_->fonts().face(font::kBn), prompt, 640, 700.111f, ink, font::slot(font::kBn).scale * 0.95f, 1);
    }
  } else if (menu_phase) {
    // Option i at PS1 (60, 269 + 32 i) = canvas (120, 576.4 + 68.57 i), slot 5 (ci_70) at the slot scale 0.6 x 1.0,
    // H2(colour, 0x14, 4): selected 0x6090B0, others 0x909090; cursor "▶" (U+25B6) in slot 6 (cur_70) at PS1 (30, 267 + 32 sel),
    // colour 0x6090B0. No panel.
    // Variant 2 (New Game+): slot 0 (kb_70) in JP, else slot 5 (ci_70), and JP y + 6 (PS1 px = 12.86 canvas).
    // Page 1 (vibration): On / Off at the same positions (no JP shift), and the footer.
    const bool style82 = page_ == 0 && variant_ == 2;
    const float y0 = 576.4f + (style82 && lang_ == import::Lang::Jp ? 6.0f * kCanvasHeight / 448.0f : 0.0f), pitch = 68.57f;
    const font::Slot face_slot = style82 && lang_ == import::Lang::Jp ? font::kKb : font::kCi;
    // Footer: JP slot 0 kb_70, else slot 5 ci_70, at PS1 (0x144, 200 / JP 206), colour 0x909090.
    if (page_ == 1) {
      const bool jp = lang_ == import::Lang::Jp;
      const font::Slot fs = jp ? font::kKb : font::kCi;
      dl.text(kit_->fonts().face(fs), kVibration[std::min(lang_index(lang_), 5)], 0x144 * 2.0f, (jp ? 206.0f : 200.0f) * kCanvasHeight / 448.0f,
              font::h2(0x909090, 0x14, 4), font::slot(fs).scale);
    }
    const float sc = font::slot(face_slot).scale;
    // The PS1 underline per option: a sprite at PS1 (0, 0x10E + 0x20 i), 32 rows from atlas cell n = page * 4 + 2 i + (selected),
    // V + 1 and width = the page-0 option text measured (PS1 pixels of the 640-wide display) + 0x50; page 1 reuses page 0's widths.
    // Page 1 also draws two footer sprites (PS1 (0x13A, 200) cell 8 and (0x23A, 200) cell 9 = the second texture page, rows 256 +,
    // width 0x100). Opaque, colour 0x80. Beyond 256 px U wraps.
    if (const render::TexRef bars = bars_[variant_ == 2 ? 1 : 0]; bars && bars_h_[variant_ == 2 ? 1 : 0] > 0) {
      const float th = bars_h_[variant_ == 2 ? 1 : 0], ps1_scale = kCanvasHeight / 448.0f;
      const int lang_idx = std::min(lang_index(lang_), 5);
      // Page 0's font slot.
      const font::Slot f0 = variant_ == 2 && lang_ == import::Lang::Jp ? font::kKb : font::kCi;
      auto sprt = [&](float x0, float y0p, int cell, float w) {
        const float v0 = (32.0f * cell + 1.0f) / th, v1 = std::min(32.0f * cell + 33.0f, th) / th;
        for (float x = 0; x < w; x += 256.0f) {
          const float pw = std::min(256.0f, w - x);
          dl.image(bars, 2.0f * (x0 + x), y0p * ps1_scale, 2.0f * pw, (v1 - v0) * th * ps1_scale, 0xFFFFFFFFu, 0.0f, v0, pw / 256.0f, v1);
        }
      };
      if (page_ == 1) {
        sprt(0x13A, 200, 8, 256.0f);
        sprt(0x23A, 200, 9, 256.0f);
      }
      for (int i = 0; i < item_count(); ++i) {
        const std::u32string p0 = variant_ == 2 ? (i == 0 ? kNewPlus[lang_idx] : kContPlus[lang_idx])
                                  : page_ == 1  ? (i == 0 ? kNew[lang_idx] : kCont[lang_idx])
                                                : label(i);
        // + 5: the original measure is 5 PS1 px wider than our advance sum for both EN strings.
        const float w = dl.measure(kit_->fonts().face(f0), p0, font::slot(f0).scale) * 0.5f + 5.0f + 80.0f;
        sprt(0, 270.0f + 32.0f * static_cast<float>(i), page_ * 4 + 2 * i + (i == sel_ ? 1 : 0), w);
      }
    }
    for (int i = 0; i < item_count(); ++i) {
      const bool sel = i == sel_;
      // A disabled Continue (no saves) is drawn darker.
      const font::Ink ink = !enabled(i) ? font::h2(0x484848, 0x14, 4) : font::h2(sel ? 0x6090B0 : 0x909090, 0x14, 4);
      dl.text(kit_->fonts().face(face_slot), label(i), 120, y0 + pitch * static_cast<float>(i), ink, sc);
    }
    dl.text(kit_->fonts().face(font::kCur), U"▶", 60, 572.1f + pitch * static_cast<float>(sel_), font::h2(0x6090B0, 0x14, 4),
            font::slot(font::kCur).scale);
  }
  // The fade (fade_alpha()) is drawn by GameFlow::draw in window space over the whole screen in use (widescreen: over the 3D sea too).
}

}  // namespace oc::ui
