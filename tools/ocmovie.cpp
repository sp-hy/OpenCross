// ocmovie: Bink 2 FMV tool.
//   ocmovie [-g <game dir>] [-l <lang>] play <name> [seconds]        window + audio, A/V synced; Esc/Enter/Space skips
//   ocmovie [-g <game dir>] [-l <lang>] dump <name> <frame> <out.png>  decode to frame N (0-based) and save it (headless)
//   ocmovie [-g <game dir>] [-l <lang>] info <name>           size, rate, length, audio peak and rms per 10 s
//   ocmovie [-g <game dir>] [-l <lang>] table             the field script's movie operand -> movie.dat member mapping of both discs
// <name> is a movie.dat member without extension ("a01op", "title"); <lang> (default en) picks a01op_<lang> etc.
// The game dir defaults to $OPENCROSS_GAMEDIR, then the exe directory and its parents.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "audio/mixer.h"
#include "core/log.h"
#include "game/disc_index.h"
#include "movie/decoder.h"
#include "movie/player.h"
#include "platform/paths.h"
#include "platform/window.h"
#include "render/sprite_batch.h"
#include "render/gpu_context.h"
#include "tool_util.h"
#include "vfs/vfs.h"

using namespace oc;

namespace {

int usage() {
  std::fprintf(stderr,
               "usage: ocmovie [-g <game dir>] [-l <lang>] play <name> [seconds]\n"
               "       ocmovie [-g <game dir>] [-l <lang>] dump <name> <frame> <out.png>\n"
               "       ocmovie [-g <game dir>] [-l <lang>] info <name>\n");
  return 2;
}

bool load_decoder(const vfs::Vfs& game_fs, const std::string& name, const std::string& lang, movie::Decoder& decoder,
                  std::string& member) {
  member = movie::resolve_member(game_fs, name, lang);
  if (member.empty()) {
    std::fprintf(stderr, "movie '%s' not found\n", name.c_str());
    return false;
  }
  auto bytes = game_fs.read(member);
  return bytes && decoder.open(std::move(*bytes));
}

int cmd_dump(const vfs::Vfs& game_fs, const std::string& lang, const std::string& name, u64 frame, const std::string& out) {
  movie::Decoder decoder;
  std::string member;
  if (!load_decoder(game_fs, name, lang, decoder, member)) return 1;
  for (;;) {
    auto& queue = decoder.video();
    while (!queue.empty() && queue.front().index < frame) queue.pop_front();
    if (!queue.empty()) {
      if (!tool::write_png(out, decoder.width(), decoder.height(), queue.front().rgba)) {
        std::fprintf(stderr, "cannot write %s\n", out.c_str());
        return 1;
      }
      std::printf("%s frame %llu (%.3f s) -> %s (%ux%u)\n", member.c_str(), static_cast<unsigned long long>(frame),
                  queue.front().pts, out.c_str(), decoder.width(), decoder.height());
      return 0;
    }
    decoder.audio().clear();
    if (!decoder.step()) break;
  }
  std::fprintf(stderr, "%s has fewer than %llu frames\n", member.c_str(), static_cast<unsigned long long>(frame + 1));
  return 1;
}

int cmd_info(const vfs::Vfs& game_fs, const std::string& lang, const std::string& name) {
  movie::Decoder decoder;
  std::string member;
  if (!load_decoder(game_fs, name, lang, decoder, member)) return 1;
  u64 frames = 0, samples = 0;
  constexpr u64 kWindow = 10ull * movie::Decoder::kAudioRate;
  std::vector<double> window_sum;
  float peak = 0.0f;
  while (decoder.step()) {
    frames += decoder.video().size();
    const auto& pcm = decoder.audio();
    for (size_t i = 0; i < pcm.size(); ++i) {
      const u64 window = (samples + i / 2) / kWindow;
      if (window >= window_sum.size()) window_sum.resize(window + 1, 0.0);
      window_sum[window] += static_cast<double>(pcm[i]) * pcm[i];
      peak = std::max(peak, std::abs(pcm[i]));
    }
    samples += pcm.size() / 2;
    decoder.video().clear();
    decoder.audio().clear();
  }
  frames += decoder.video().size();
  std::printf("%s: %ux%u @ %.3f fps, %llu frames (%.2f s), audio %s %.2f s\n", member.c_str(), decoder.width(), decoder.height(),
              decoder.fps(), static_cast<unsigned long long>(frames), static_cast<double>(frames) / decoder.fps(),
              decoder.has_audio() ? "yes" : "no", static_cast<double>(samples) / movie::Decoder::kAudioRate);
  if (samples == 0) return 0;
  const auto db = [](double power) { return power > 0.0 ? 10.0 * std::log10(power) : -999.0; };
  std::printf("audio peak %.1f dBFS, rms per 10 s (dBFS):", 20.0 * std::log10(std::max(peak, 1e-12f)));
  for (size_t i = 0; i < window_sum.size(); ++i) {
    const u64 count = std::min<u64>(kWindow, samples - i * kWindow) * 2;
    std::printf(" %.1f", db(window_sum[i] / static_cast<double>(count)));
  }
  std::printf("\n");
  return 0;
}

// The low byte of the field script's movie operand indexes the disc's movie table: row -> movie number -> the fnd record
// DiscIndex::kMovieBase + number, whose name resolves to a movie.dat member.
int cmd_table(const vfs::Vfs& game_fs, const std::string& lang) {
  game::DiscIndex index;
  if (!index.load(game_fs)) {
    std::fprintf(stderr, "cdrom.fnd missing\n");
    return 1;
  }
  for (int disc = 1; disc <= 2; ++disc) {
    std::printf("disc %d (field.prg table 0x%s)\n op  num  start   end vol  record                       movie.dat member\n", disc, disc == 1 ? "800E8184" : "800E83C4");
    for (int op = 0; op < 24; ++op) {
      const auto* row = game::DiscIndex::movie_row(disc, op);
      if (!row) break;
      const std::string name = index.movie_name(disc, op);
      std::printf(" %2d  %3d  %5d %5d %3d  %-28s %s\n", op, row->number, row->start, row->end, row->volume,
                  index.record(disc, game::DiscIndex::kMovieBase + row->number).c_str(),
                  name.empty() ? "(no movie)" : movie::resolve_member(game_fs, name, lang).c_str());
    }
  }
  return 0;
}

int cmd_play(const vfs::Vfs& game_fs, const std::string& lang, const std::string& name, double seconds) {
  platform::Window window;
  if (!window.create("ocmovie", 960, 720, false)) return 1;
  render::GpuContext ctx;
  if (!ctx.init(window.handle(), true)) return 1;
  ctx.set_clear_color(0, 0, 0);
  render::SpriteBatch sprites;
  if (!sprites.init(ctx)) return 1;

  audio::Mixer mixer;
  const bool audio_ok = mixer.open_device();
  if (!audio_ok) log::warn("no audio device; playing silent on the wall clock");

  movie::Player player;
  if (audio_ok) player.set_mixer(&mixer);
  if (!player.open(game_fs, name, lang) || !player.init_gpu(ctx, sprites)) return 1;

  platform::Input input;
  platform::FrameTimer timer;
  double elapsed = 0;
  u32 presented = 0;
  while (window.pump(input) && !player.finished()) {
    const double dt = timer.tick();
    elapsed += dt;
    if (input.key_pressed[SDL_SCANCODE_ESCAPE] || input.key_pressed[SDL_SCANCODE_RETURN] ||
        input.key_pressed[SDL_SCANCODE_SPACE])
      player.skip();
    if (seconds > 0 && elapsed >= seconds) break;
    player.update(dt);
    render::Frame frame;
    if (!ctx.begin_frame(frame)) {
      SDL_Delay(10);
      continue;
    }
    player.draw(frame, static_cast<float>(frame.extent.width), static_cast<float>(frame.extent.height));
    ctx.end_frame();
    ++presented;
  }
  std::printf("played %.2f s wall, movie clock %.2f s, last frame %llu, %u presents%s\n", elapsed, player.clock(),
              static_cast<unsigned long long>(player.frame_index()), presented, player.finished() ? " (finished)" : "");
  player.close();
  mixer.close_device();
  return 0;
}

}  // namespace

int ocmovie_main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  std::string game_dir = tool::env_game_dir();
  std::string lang = "en";
  while (args.size() >= 2 && (args[0] == "-g" || args[0] == "-l")) {
    (args[0] == "-g" ? game_dir : lang) = args[1];
    args.erase(args.begin(), args.begin() + 2);
  }
  if (args.empty() || (args.size() < 2 && args[0] != "table")) return usage();
  const auto dir = platform::find_game_dir(game_dir);
  vfs::Vfs game_fs;
  if (dir.empty() || !game_fs.open(dir)) {
    std::fprintf(stderr, "game dir not found (use -g or OPENCROSS_GAMEDIR)\n");
    return 1;
  }
  const std::string& cmd = args[0];
  if (cmd == "play") return cmd_play(game_fs, lang, args[1], args.size() > 2 ? std::atof(args[2].c_str()) : 0.0);
  if (cmd == "dump" && args.size() == 4)
    return cmd_dump(game_fs, lang, args[1], std::strtoull(args[2].c_str(), nullptr, 10), args[3]);
  if (cmd == "table") return cmd_table(game_fs, lang);
  if (cmd == "info") return cmd_info(game_fs, lang, args[1]);
  return usage();
}
