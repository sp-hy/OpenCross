// Ends the game process: the teardown watchdog and the hand-over to the exe started in this one's place (process_exit.h).
#include "platform/process_exit.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "core/log.h"
#include "platform/paths.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace oc::platform {

namespace {

constexpr int kTeardownLimitSeconds = 10;

std::mutex g_mutex;
std::condition_variable g_torn_down;
bool g_armed = false;
bool g_done = false;
bool g_successor = false;
int g_exit_code = 0;
// Held by the first caller of end_now until the process is gone, so the watchdog and the main thread never both run it.
std::mutex g_ending;

#ifdef _WIN32
// Starts the exe with this process's command line and waits for it; its exit code, or `fallback` when it could not start. The log
// file is closed once it runs: the new process opens it afresh.
int run_successor(int fallback) {
  const std::filesystem::path exe = exe_path();
  std::wstring cmd = GetCommandLineW();
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (exe.empty() || !CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
    log::error("update: restart failed ({})", GetLastError());
    return fallback;
  }
  log::info("update: restarted; this process waits for the new one to exit");
  log::close();
  CloseHandle(pi.hThread);
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 0;
  const int result = GetExitCodeProcess(pi.hProcess, &code) ? static_cast<int>(code) : fallback;
  CloseHandle(pi.hProcess);
  return result;
}
#endif

[[noreturn]] void end_now() {
  std::lock_guard ending(g_ending);
  int code = 0;
  bool successor = false;
  {
    std::lock_guard lock(g_mutex);
    code = g_exit_code;
    successor = g_successor;
  }
#ifdef _WIN32
  if (successor) code = run_successor(code);
  TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
#endif
  std::_Exit(code);
}

}  // namespace

void begin_exit(int exit_code, bool start_successor) {
  {
    std::lock_guard lock(g_mutex);
    if (g_armed) return;
    g_armed = true;
    g_exit_code = exit_code;
    g_successor = start_successor;
  }
  std::thread([] {
    {
      std::unique_lock lock(g_mutex);
      if (g_torn_down.wait_for(lock, std::chrono::seconds(kTeardownLimitSeconds), [] { return g_done; })) return;
    }
    log::warn("exit: shutdown did not finish within {} s; ending the process", kTeardownLimitSeconds);
    end_now();
  }).detach();
}

void finish_exit() {
  {
    std::lock_guard lock(g_mutex);
    g_done = true;
  }
  g_torn_down.notify_all();
  end_now();
}

}  // namespace oc::platform
