#pragma once
// Continue: the save choice (OpenCross / Slot 1 / Slot 2), then the OpenCross page (an auto-save + 100 files, a scrolling list) or
// that memory card's slot grid (15 slots per card, CHRONOnn, slot 00 = the RDE auto-save). Confirm on a choice opens it ("No save
// data" when it is empty); confirm on a file picks it; cancel on a list goes back to the choice, cancel on the choice leaves. The
// choice starts on OpenCross when the page holds a file, else on Slot 1.
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "save/cc_save.h"
#include "save/oc_saves.h"
#include "ui/title/canvas.h"
#include "ui/title/nav.h"

namespace oc::ui {

struct SlotEntry {
  // card: the memory card (1 or 2), 0 = the OpenCross page; slot: the CHRONOnn number (page: the file number, 0 = its auto-save);
  // block: the memory-card block (page: 0).
  int card = 1;
  int slot = 0;
  int block = 0;
  save::SaveSlot data;
  // Its ext_save.dat names in the list's language (filled by SlotList::begin).
  std::map<int, std::u32string> ext_names;
};

class SlotList {
 public:
  // All decodable slots of `card` (empty when the card is absent), in the card's directory order (= box order).
  static std::vector<SlotEntry> collect(const save::SaveDat& dat, int card = 1);
  // Every file of the OpenCross page, indexed by number (0 = its auto-save).
  static std::vector<std::optional<SlotEntry>> collect(const save::OcSaves& page);
  // True when either card or the page holds a file.
  static bool any(const save::SaveDat* dat, const save::OcSaves* page);

  // Starts on the save choice with the files of both cards of `dat` and of `page` (none when null).
  // `plus`: the Continue+ list of the New Game+ title: slots whose 2 summary flag bits are set (a
  // cleared save or a New Game+ game) show the menutest message instead of their details and are refused (sfx 0x18).
  void begin(const save::SaveDat* dat, const save::OcSaves* page, import::Lang lang, ArtMode mode, bool plus = false);
  bool plus() const { return plus_; }
  void set_mode(ArtMode mode) { mode_ = mode; }
  // A character's name in a saved game: its data, its ext_save.dat names, the character id.
  using NameLookup = std::function<std::u32string(const save::SaveSlot&, const std::map<int, std::u32string>&, int)>;
  void set_name_lookup(NameLookup fn) { name_of_ = std::move(fn); }
  void set_sfx(std::function<void(int)> fn) { sfx_ = std::move(fn); }

  void update(const Nav& nav, float dt);
  bool picked() const { return picked_; }
  bool cancelled() const { return cancelled_; }
  // The file list of the chosen card is open (false on the card choice and its "No save data" message).
  bool listing() const { return phase_ == Phase::Files; }
  const SlotEntry* selected() const {
    if (phase_ != Phase::Files) return nullptr;
    if (card_ == 0) return rows_[static_cast<size_t>(sel_)] ? &*rows_[static_cast<size_t>(sel_)] : nullptr;
    return entries_.empty() ? nullptr : &entries_[static_cast<size_t>(sel_)];
  }
  int selected_index() const { return sel_; }
  size_t size() const { return entries_.size(); }
  void draw(DrawList& dl, UiKit& kit) const;

 private:
  void play(int id) {
    if (sfx_) sfx_(id);
  }
  struct NamePic {
    render::TexRef set = {};
    float w = 0, h = 0;
  };
  mutable std::map<std::string, NamePic> pics_;
  // Image cache vs the mod overlay: when Vfs::generation() moves (Mods tab / art mode Modded), every cached picture is decoded again from
  // the re-pointed files; a same-sized one is refreshed in place (UiKit::update_rgba), others re-uploaded.
  mutable std::map<std::string, NamePic> stale_;
  mutable std::uint32_t vfs_gen_ = 0;
  mutable std::map<int, std::string> face_files_;
  // Which face folder face_files_ lists: 0 none, 1 = face/2021 (HD), 2 = face/1999 (original art).
  mutable int faces_listed_ = 0;
  mutable std::unique_ptr<import::LangTables> tables_;
  // Card: OpenCross / Slot 1 / Slot 2 choice; NoData: "No save data in ..." (confirm or 2 s back to the choice); Files: the list.
  enum class Phase { Card, NoData, Files };
  Phase phase_ = Phase::Card;
  // card_: 0 = the OpenCross page, 1 = Slot 1, 2 = Slot 2; entries_: the files of a card once chosen; rows_: the page by number.
  // sel_: grid box, or page row (= file number); top_: the page's first visible row.
  int card_ = 1;
  float phase_time_ = 0, clock_ = 0;
  std::array<std::vector<SlotEntry>, 2> cards_;
  std::vector<SlotEntry> entries_;
  std::vector<std::optional<SlotEntry>> rows_ = std::vector<std::optional<SlotEntry>>(save::kOcFiles + 1);
  import::Lang lang_ = import::Lang::En;
  ArtMode mode_ = ArtMode::Hd;
  int sel_ = 0, top_ = 0;
  bool picked_ = false, cancelled_ = false;
  bool plus_ = false;
  NameLookup name_of_;
  std::function<void(int)> sfx_;
};

// Play time as "H:MM:SS".
std::u32string format_playtime(u32 frames);

}  // namespace oc::ui
