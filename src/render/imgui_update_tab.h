#pragma once
// Esc overlay tab "Update": a button to the OpenCross Nexus Mods page, the running version (core/version.h = the exe's version
// resource) and the GitHub release check / download / install of platform::Updater.
namespace oc::platform {
class Updater;
}

namespace oc::render {

// Draws the tab body. *restart is set when the player asked to restart into the installed update (the app leaves its main loop, then
// Updater::restart()).
void draw_update_tab(platform::Updater& updater, bool* restart);
// A newer OpenCross release waits (found, downloading, or installed and waiting for a restart): the tab label is highlighted.
bool update_pending(const platform::Updater& updater);

}  // namespace oc::render
