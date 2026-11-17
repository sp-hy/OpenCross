// The disc-change screen's state machine: fade-in, waiting for a change, a wrong disc (the "wrong disc" line and the way back to
// the prompt), the right disc (fade-out, closed), and the PC actions (Rescan, Browse). No GPU: init() is not called.
#include <cstdio>
#include <vector>

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

}  // namespace

int main() {
  test_wrong_then_right();
  test_pc_actions();
  test_open_rules();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("disc prompt: ok\n");
  return 0;
}
