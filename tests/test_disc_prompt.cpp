// The disc-change screen's state machine: fade-in, waiting for a change, a wrong disc (the "wrong disc" line and the way back to
// the prompt), the right disc (fade-out, closed), and the PC actions (Rescan, Browse). Then the sources behind it, synthetic: any media
// change of a drive or an image folder brings an attempt, and the discs refused (no disc, another build, the other disc, another game)
// each show the "wrong disc" line before the right one is taken. No GPU: init() is not called.
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "import/disc/disc_search.h"
#include "import/disc/drive_source.h"
#include "ui/disc_prompt.h"

using namespace oc;
using ui::DiscPrompt;

namespace {

int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

struct FakeHost {
  std::vector<DiscPrompt::MountResult> answers;
  std::vector<int> asked;
  std::vector<int> browsed;
  bool change = false;

  DiscPrompt::Host host() {
    DiscPrompt::Host out;
    out.try_mount = [this](int disc) {
      asked.push_back(disc);
      if (answers.empty()) return DiscPrompt::MountResult::NoDisc;
      const DiscPrompt::MountResult result = answers.front();
      answers.erase(answers.begin());
      return result;
    };
    out.changed = [this] {
      const bool was = change;
      change = false;
      return was;
    };
    out.browse = [this](int disc) { browsed.push_back(disc); };
    return out;
  }
};

void run(DiscPrompt& prompt, int frames, const DiscPrompt::Input& input = {}) {
  for (int i = 0; i < frames; ++i) prompt.update(input);
}

void test_wrong_then_right() {
  FakeHost fake;
  fake.answers = {DiscPrompt::MountResult::WrongDisc, DiscPrompt::MountResult::Ok};
  DiscPrompt prompt;
  prompt.set_host(fake.host());
  CHECK(!prompt.active());
  prompt.open(2);
  CHECK(prompt.active());
  CHECK(prompt.needed_disc() == 2);
  CHECK(prompt.phase() == DiscPrompt::Phase::FadeIn);
  CHECK(prompt.level() == 0);
  CHECK(prompt.line() == DiscPrompt::Line::Prompt);
  run(prompt, 1);
  CHECK(prompt.level() == 4);
  run(prompt, 30);
  CHECK(prompt.level() == 124);
  CHECK(prompt.phase() == DiscPrompt::Phase::FadeIn);
  run(prompt, 1);
  CHECK(prompt.phase() == DiscPrompt::Phase::Waiting);
  CHECK(prompt.level() == 124);
  // Nothing changes, nothing is tried: the PS1 waits for the lid.
  run(prompt, 600);
  CHECK(prompt.phase() == DiscPrompt::Phase::Waiting);
  CHECK(fake.asked.empty());
  // A change with the wrong disc: 126 frames of "wrong disc", 4 of the prompt, then waiting at 128.
  fake.change = true;
  run(prompt, 1);
  CHECK(fake.asked.size() == 1 && fake.asked[0] == 2);
  CHECK(prompt.phase() == DiscPrompt::Phase::Error);
  CHECK(prompt.last_result() == DiscPrompt::MountResult::WrongDisc);
  CHECK(prompt.line() == DiscPrompt::Line::WrongDisc);
  CHECK(prompt.level() == 128);
  run(prompt, 125);
  CHECK(prompt.line() == DiscPrompt::Line::WrongDisc);
  run(prompt, 1);
  CHECK(prompt.line() == DiscPrompt::Line::Prompt);
  CHECK(prompt.phase() == DiscPrompt::Phase::Error);
  run(prompt, 3);
  CHECK(prompt.phase() == DiscPrompt::Phase::Error);
  run(prompt, 1);
  CHECK(prompt.phase() == DiscPrompt::Phase::Waiting);
  CHECK(prompt.level() == 128);
  CHECK(fake.asked.size() == 1);
  // The right disc: the veil lifts from 124 over 31 frames without text, then the screen closes.
  fake.change = true;
  run(prompt, 1);
  CHECK(prompt.phase() == DiscPrompt::Phase::FadeOut);
  CHECK(prompt.last_result() == DiscPrompt::MountResult::Ok);
  CHECK(prompt.level() == 124);
  CHECK(prompt.line() == DiscPrompt::Line::None);
  run(prompt, 30);
  CHECK(prompt.level() == 4);
  CHECK(prompt.active());
  run(prompt, 1);
  CHECK(!prompt.active());
  CHECK(prompt.level() == 0);
  CHECK(fake.asked.size() == 2);
}

void test_pc_actions() {
  FakeHost fake;
  fake.answers = {DiscPrompt::MountResult::WrongBuild, DiscPrompt::MountResult::Ok};
  DiscPrompt prompt;
  prompt.set_host(fake.host());
  prompt.open(1);
  // Browse during the fade-in opens the dialog at once; a Rescan then is kept and tried when the fade-in ends.
  DiscPrompt::Input browse;
  browse.browse = true;
  prompt.update(browse);
  CHECK(fake.browsed.size() == 1 && fake.browsed[0] == 1);
  DiscPrompt::Input rescan;
  rescan.rescan = true;
  prompt.update(rescan);
  CHECK(fake.asked.empty());
  run(prompt, 30);
  CHECK(fake.asked.size() == 1 && fake.asked[0] == 1);
  CHECK(prompt.phase() == DiscPrompt::Phase::Error);
  CHECK(prompt.last_result() == DiscPrompt::MountResult::WrongBuild);
  CHECK(prompt.line() == DiscPrompt::Line::WrongDisc);
  // A Rescan during the "wrong disc" line is tried once the prompt waits again.
  prompt.update(rescan);
  run(prompt, 128);
  CHECK(prompt.phase() == DiscPrompt::Phase::Error);
  run(prompt, 1);
  CHECK(prompt.phase() == DiscPrompt::Phase::FadeOut);
  CHECK(fake.asked.size() == 2);
  run(prompt, 31);
  CHECK(!prompt.active());
}

void test_open_rules() {
  DiscPrompt prompt;
  prompt.open(2);
  run(prompt, 40);
  CHECK(prompt.phase() == DiscPrompt::Phase::Waiting);
  // Opening again for the same disc keeps the screen as it is; another disc starts it over.
  prompt.open(2);
  CHECK(prompt.phase() == DiscPrompt::Phase::Waiting);
  prompt.open(1);
  CHECK(prompt.phase() == DiscPrompt::Phase::FadeIn);
  CHECK(prompt.needed_disc() == 1);
  // No host: a Rescan finds nothing.
  run(prompt, 32);
  DiscPrompt::Input rescan;
  rescan.rescan = true;
  prompt.update(rescan);
  CHECK(prompt.phase() == DiscPrompt::Phase::Error);
  CHECK(prompt.last_result() == DiscPrompt::MountResult::NoDisc);
}

// A drive and an image folder as the app watches them: every change of either is noticed, whatever disc it brings, and an attempt
// picks the needed disc of the played build among what they hold.
struct FakeSources {
  std::vector<platform::OpticalDriveInfo> drives{{"D:", "VIRTUAL DRIVE", true, 1u, "CC1"}};
  std::vector<import::disc::FoundDisc> drive_discs, image_discs;
  std::string images;
  // The drive's media-change count.
  u32 changes = 1;
  std::string seen = signature();
  std::vector<DiscPrompt::MountResult> results;

  std::string signature() const { return import::disc::drive_media_signature(drives) + images; }
  void put_in_drive(std::vector<import::disc::FoundDisc> discs, const std::string& label) {
    platform::OpticalDriveInfo& drive = drives[0];
    drive.media = !discs.empty();
    drive.generation = drive.media ? std::optional<u32>(++changes) : std::nullopt;
    drive.label = label;
    drive_discs = std::move(discs);
  }

  DiscPrompt::Host host(const std::string& build) {
    DiscPrompt::Host out;
    out.try_mount = [this, build](int disc) {
      std::vector<import::disc::FoundDisc> found = image_discs;
      found.insert(found.end(), drive_discs.begin(), drive_discs.end());
      DiscPrompt::MountResult result = DiscPrompt::MountResult::Ok;
      switch (import::disc::pick_disc(found, disc, build).fit) {
        case import::disc::DiscFit::NoDisc:
          result = DiscPrompt::MountResult::NoDisc;
          break;
        case import::disc::DiscFit::NotChronoCross:
          result = DiscPrompt::MountResult::NotGame;
          break;
        case import::disc::DiscFit::OtherDisc:
          result = DiscPrompt::MountResult::WrongDisc;
          break;
        case import::disc::DiscFit::OtherBuild:
          result = DiscPrompt::MountResult::WrongBuild;
          break;
        case import::disc::DiscFit::Fits:
          break;
      }
      results.push_back(result);
      return result;
    };
    out.changed = [this] {
      std::string now = signature();
      const bool changed = now != seen;
      seen = std::move(now);
      return changed;
    };
    return out;
  }
};

import::disc::FoundDisc chrono_cross_disc(const std::string& build, u32 number) {
  import::disc::FoundDisc disc;
  disc.where = "D: (VIRTUAL DRIVE)";
  disc.identity.chrono_cross = true;
  disc.identity.known_build = true;
  disc.identity.build = build;
  disc.identity.disc_number = number;
  return disc;
}

import::disc::FoundDisc other_disc() {
  import::disc::FoundDisc disc;
  disc.where = "D: (VIRTUAL DRIVE)";
  disc.identity.reason = "no SYSTEM.CNF";
  return disc;
}

// One refused disc: the attempt happens on the change, shows the "wrong disc" line, and the screen waits again.
void expect_refused(DiscPrompt& prompt, FakeSources& sources, DiscPrompt::MountResult expected) {
  const size_t before = sources.results.size();
  run(prompt, 1);
  CHECK(sources.results.size() == before + 1);
  CHECK(prompt.last_result() == expected);
  CHECK(prompt.phase() == DiscPrompt::Phase::Error);
  CHECK(prompt.line() == DiscPrompt::Line::WrongDisc);
  run(prompt, DiscPrompt::kErrorFrames + DiscPrompt::kRecoverFrames);
  CHECK(prompt.phase() == DiscPrompt::Phase::Waiting);
  // Nothing more changes: no further attempt.
  run(prompt, 300);
  CHECK(sources.results.size() == before + 1);
}

void test_drive_signature() {
  std::vector<platform::OpticalDriveInfo> drives{{"D:", "", true, 3u, "CC1"}};
  const std::string first = import::disc::drive_media_signature(drives);
  drives[0].generation = 4u;
  const std::string swapped = import::disc::drive_media_signature(drives);
  CHECK(swapped != first);
  drives[0].media = false, drives[0].generation.reset(), drives[0].label.clear();
  const std::string empty = import::disc::drive_media_signature(drives);
  CHECK(empty != swapped);
  drives[0].media = true, drives[0].generation = 5u;
  const std::string unread = import::disc::drive_media_signature(drives);
  CHECK(unread != empty);
  // The same count with a label: the disc became readable after it spun up.
  drives[0].label = "CC2";
  CHECK(import::disc::drive_media_signature(drives) != unread);
  CHECK(import::disc::drive_media_signature(drives) == import::disc::drive_media_signature(drives));
}

void test_pick_disc() {
  using import::disc::DiscFit;
  const std::string usa = "USA";
  CHECK(import::disc::pick_disc({}, 2, usa).fit == DiscFit::NoDisc);
  CHECK(import::disc::pick_disc({}, 2, usa).index == -1);
  CHECK(import::disc::pick_disc({other_disc()}, 2, usa).fit == DiscFit::NotChronoCross);
  CHECK(import::disc::pick_disc({chrono_cross_disc(usa, 1)}, 2, usa).fit == DiscFit::OtherDisc);
  CHECK(import::disc::pick_disc({chrono_cross_disc("Japan", 2)}, 2, usa).fit == DiscFit::OtherBuild);
  // The closest miss is reported; the disc that fits is picked wherever it sits.
  CHECK(import::disc::pick_disc({other_disc(), chrono_cross_disc("Japan", 2), chrono_cross_disc(usa, 1)}, 2, usa).fit == DiscFit::OtherBuild);
  const import::disc::DiscPick pick =
      import::disc::pick_disc({chrono_cross_disc("Japan", 2), chrono_cross_disc(usa, 2), chrono_cross_disc(usa, 1)}, 2, usa);
  CHECK(pick.index == 1 && pick.fit == DiscFit::Fits);
}

// Disc 2 of the USA build is asked for with disc 1 in the drive: it leaves (no disc), then another build's disc 2, disc 1 again, a disc
// of another game and, in the image folder, the other build's disc 2 are each tried and refused, and the right disc closes the screen.
void test_refused_discs() {
  const std::string usa = "USA";
  FakeSources sources;
  sources.drive_discs = {chrono_cross_disc(usa, 1)};
  DiscPrompt prompt;
  prompt.set_host(sources.host(usa));
  prompt.open(2);
  run(prompt, DiscPrompt::kFadeInFrames);
  CHECK(prompt.phase() == DiscPrompt::Phase::Waiting);
  CHECK(sources.results.empty());
  sources.put_in_drive({}, "");
  expect_refused(prompt, sources, DiscPrompt::MountResult::NoDisc);
  sources.put_in_drive({chrono_cross_disc("Japan", 2)}, "CC2");
  expect_refused(prompt, sources, DiscPrompt::MountResult::WrongBuild);
  sources.put_in_drive({chrono_cross_disc(usa, 1)}, "CC1");
  expect_refused(prompt, sources, DiscPrompt::MountResult::WrongDisc);
  sources.put_in_drive({other_disc()}, "OTHER");
  expect_refused(prompt, sources, DiscPrompt::MountResult::NotGame);
  sources.image_discs = {chrono_cross_disc("Japan", 2)};
  sources.images = "Chrono Cross (Japan) (Disc 2).cue|1/1\n";
  expect_refused(prompt, sources, DiscPrompt::MountResult::WrongBuild);
  sources.put_in_drive({chrono_cross_disc(usa, 2)}, "CC2");
  run(prompt, 1);
  CHECK(prompt.last_result() == DiscPrompt::MountResult::Ok);
  CHECK(prompt.phase() == DiscPrompt::Phase::FadeOut);
  run(prompt, DiscPrompt::kFadeOutFrames);
  CHECK(!prompt.active());
  CHECK(sources.results.size() == 6);
}

}  // namespace

int main() {
  test_wrong_then_right();
  test_pc_actions();
  test_open_rules();
  test_drive_signature();
  test_pick_disc();
  test_refused_discs();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("disc prompt: ok\n");
  return 0;
}
