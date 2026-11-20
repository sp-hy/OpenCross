// OpenCross entry point: `opencross <tool> [args]` runs a developer tool; anything else boots the game (app/app.h).
#include <SDL3/SDL_main.h>

#include "app/app.h"
#include "platform/process_exit.h"

// Defined in tool_main.cpp.
int run_tool(int argc, char** argv);

int main(int argc, char** argv) {
  if (const int rc = run_tool(argc, argv); rc >= 0) return rc;
  {
    oc::app::App app;
    if (const int rc = app.init(argc, argv); rc != 0) return rc;
    app.run();
  }
  oc::platform::finish_exit();
}
