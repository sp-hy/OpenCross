#pragma once
// The "insert the other disc" screen of disc mode, as the PS1 field shows it when script variable 0x42 names a disc that is not in
// the drive (after FE 65, or after loading a save made on the other disc).
//
// The picture stays frozen while a subtractive grey veil darkens it by 4 per 60 Hz frame from 0 to 124 (32 frames); a "wrong disc"
// line raises it to 128. Over it, at once, the prompt line of map/etc/systim.bin TIM 5 ("DISC n ..." / "PLEASE INSERT DISC n", the digit pasted in
// from the same TIM). Nothing reads the pad. A source is tried only when something changes (the PC's lid cycle: a drive's media, a
// new image, a picked file, or the Rescan action); a failure of any kind shows the "wrong disc" line for 126 frames and the prompt
// for 4 more, then waits again. The right disc lifts the veil by 4 per frame from 124 (31 frames, no text) and the screen closes;
// the game resumes where it stopped.
//
// PC additions: "Browse for image..." and "Rescan" (exe folder, configured paths, drives), shown as one dim hint line at the bottom.
//
//   DiscPrompt prompt;  prompt.init(ctx, vfs, sprites, text);  prompt.set_layout(DiscPrompt::Layout::Usa);  prompt.set_host(host);
//   prompt.open(2);  per frame while prompt.active(): prompt.update({browse, rescan});  prompt.draw(frame, vw, vh, true);
#include <functional>
#include <string>
#include <vector>

#include "render/sprite_batch.h"
#include "render/text_batch.h"
#include "render/texture.h"
#include "ui/ps1_font.h"
#include "vfs/vfs.h"

namespace oc::ui {

class DiscPrompt {
 public:
  enum class MountResult { Ok, NoDisc, WrongDisc, WrongBuild };
  // The build whose screen layout is drawn (the two builds place the lines and the digit differently).
  enum class Layout { Japan, Usa };
  enum class Phase { Closed, FadeIn, Waiting, Error, FadeOut };

  // What the app provides. Every member may be empty (then: nothing to try, nothing changes, no dialog).
  struct Host {
    // Looks for disc `disc` in every source (configured paths, images beside the exe, drives, picked files) and mounts it.
    std::function<MountResult(int disc)> try_mount;
    // True once when something new may hold the disc since the last call: a drive's media changed, an image appeared, the browse
    // dialog returned a file.
    std::function<bool()> changed;
    // Opens the "Browse for image..." dialog for disc `disc`; a pick reaches the prompt through `changed`.
    std::function<void(int disc)> browse;
  };
  // Edges of the PC actions for one frame.
  struct Input {
    bool browse = false;
    bool rescan = false;
  };

  // Frame counts of the PS1 screen (60 Hz frames).
  static constexpr int kFadeInFrames = 32, kErrorFrames = 126, kRecoverFrames = 4, kFadeOutFrames = 31;
  static constexpr int kLevelStep = 4, kHoldLevel = 128;

  DiscPrompt() = default;
  ~DiscPrompt() { shutdown(); }
  DiscPrompt(const DiscPrompt&) = delete;
  DiscPrompt& operator=(const DiscPrompt&) = delete;

  // Reads the art from the disc in the drive (cdrom/map/etc/systim.bin); false only on a GPU failure. Without the art the screen
  // still darkens and shows the hint line.
  bool init(render::GpuContext& ctx, const vfs::Vfs& vfs, render::SpriteBatch& sprites, render::TextBatch& text);
  void shutdown();
  void set_layout(Layout layout) { layout_ = layout; }
  void set_host(Host host) { host_ = std::move(host); }
  // The hint line's text (the app names its bindings); empty hides the line.
  void set_hint(std::u32string hint) { hint_ = std::move(hint); }

  // Starts the screen for disc 1 or 2 (ignored while it is already up for the same disc).
  void open(int needed_disc);
  bool active() const { return phase_ != Phase::Closed; }
  // One 60 Hz frame.
  void update(const Input& input = {});
  // Over the frozen picture (inside no begin / end); `resume` continues the frame's sprite pass.
  void draw(const render::Frame& frame, float vw, float vh, bool resume = false);

  Phase phase() const { return phase_; }
  int needed_disc() const { return disc_; }
  // Strength of the veil this frame (0..128, subtracted from each 8-bit channel).
  int level() const;
  // Which line is shown this frame.
  enum class Line { None, Prompt, WrongDisc };
  Line line() const;
  // Result of the last attempt (Ok before any).
  MountResult last_result() const { return last_; }

 private:
  void attempt();
  void build_art();

  Host host_;
  Layout layout_ = Layout::Japan;
  Phase phase_ = Phase::Closed;
  int disc_ = 0;
  int frame_ = 0;
  // The veil while waiting: the last frame drawn before the wait (124 after the fade-in, 128 after a "wrong disc" line).
  int held_level_ = 0;
  bool pending_ = false;
  MountResult last_ = MountResult::Ok;
  std::u32string hint_ = U"Browse for image...    Rescan";

  render::GpuContext* ctx_ = nullptr;
  render::SpriteBatch* sprites_ = nullptr;
  render::Sampler nearest_;
  render::Texture white_, art_;
  render::TexRef white_set_ = {}, art_set_ = {};
  // TIM 5 decoded to RGBA (256 x 32), kept to paste the digit for either layout.
  std::vector<u8> tim_rgba_;
  int built_layout_ = -1;
  font::FontSet fonts_;
};

}  // namespace oc::ui
