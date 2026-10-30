// `opencross <tool> [args]`: the developer tools live in the game exe (one shipped binary). The tool sees argv[0] = its name.
// The exe is a GUI-subsystem program, so a tool attaches to the calling console when stdout is not already redirected;
// from PowerShell pipe the output (`opencross occ rooms-check | Out-Host`) to make the shell wait for it.
#include <windows.h>

#include <cstdio>
#include <string_view>

int occ_main(int, char**);
int octext_main(int, char**);
int ocaudio_main(int, char**);
int ochd_main(int, char**);
int ocprd_main(int, char**);
int ocstage_main(int, char**);
int ocbattle_main(int, char**);
int ocmovie_main(int, char**);
int ocbattleview_main(int, char**);
int ocbattleseq_main(int, char**);
int ocflow_main(int, char**);
int ocmenudemo_main(int, char**);
int ocshopdemo_main(int, char**);
int ocmodelview_main(int, char**);

namespace {

struct Tool {
  // The tool name; also accepted without the "oc" prefix.
  const char* name;
  int (*run)(int, char**);
  const char* what;
};
constexpr Tool kTools[] = {
    {"occ", occ_main, "rooms / models / scripts / effects / saves: list, extract, decode, validate"},
    {"octext", octext_main, "message viewer (lang.dat / PS1 dialogue)"},
    {"ocaudio", ocaudio_main, "decode / play BGM"},
    {"ochd", ochd_main, "hd.dat .tim validator"},
    {"ocprd", ocprd_main, "room .prd effects: info, validate, soak, sprite PNGs"},
    {"ocstage", ocstage_main, "battle stage inspector / exporter / validator"},
    {"ocbattle", ocbattle_main, "battle tables, headless simulator, formulas, soaks"},
    {"ocmovie", ocmovie_main, "play / dump / inspect FMVs"},
    {"ocbattleview", ocbattleview_main, "one battle in a window"},
    {"ocbattleseq", ocbattleseq_main, "battle model animation-sequence dump"},
    {"ocflow", ocflow_main, "title / name entry / slot list screens in a window"},
    {"ocmenudemo", ocmenudemo_main, "main menu in a window (tmp/save.dat slot 4)"},
    {"ocshopdemo", ocshopdemo_main, "shops in a window (tmp/save.dat slot 4)"},
    {"ocmodelview", ocmodelview_main, "character models in a window: orbit, animation clips, original / modded"},
};

void attach_console() {
  const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  // Output already redirected: keep it.
  if (out != nullptr && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_UNKNOWN) return;
  if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
  FILE* stream = nullptr;
  freopen_s(&stream, "CONOUT$", "w", stdout);
  freopen_s(&stream, "CONOUT$", "w", stderr);
}

}  // namespace

// Returns the tool's exit code, or -1 when argv[1] names no tool (the game runs).
int run_tool(int argc, char** argv) {
  if (argc < 2) return -1;
  const std::string_view arg = argv[1];
  if (arg == "tools" || arg == "--tools") {
    attach_console();
    std::printf("opencross <tool> [args]\n");
    for (const Tool& tool : kTools) std::printf("  %-14s %s\n", tool.name, tool.what);
    return 0;
  }
  for (const Tool& tool : kTools) {
    const std::string_view name = tool.name;
    if (arg == name || arg == name.substr(2)) {
      attach_console();
      return tool.run(argc - 1, argv + 1);
    }
  }
  return -1;
}
