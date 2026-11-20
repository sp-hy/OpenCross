#pragma once
// How the game process ends. Teardown after the main loop (GPU, audio, Steam, library unloading) can stall under Proton and leave a
// window that never closes, so begin_exit() arms a watchdog that ends the process when teardown has not finished in time, and
// finish_exit() ends it without unloading libraries. With a successor (the updater's "Restart now"), finish_exit() starts the new exe
// with the same command line and keeps this process, windowless, until it exits, then ends with its exit code: RDE's launcher waits for
// the game it started and would otherwise come back over the new one.
namespace oc::platform {

void begin_exit(int exit_code, bool start_successor);
[[noreturn]] void finish_exit();

}  // namespace oc::platform
