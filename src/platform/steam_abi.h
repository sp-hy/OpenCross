#pragma once
// The running Steam client's library as SteamClient and SteamBrowser reach it (Windows only): where the library is, the exports and
// the client object's function slots they use, and the message a pipe delivers. Nothing from the Steamworks SDK is built in.
#ifdef _WIN32
#include <cstdint>
#include <string>

namespace oc::platform::steam_abi {

inline constexpr const char* kClientInterface = "SteamClient017";

// Slots in the client object's table of functions.
enum ClientSlot { kCreatePipe = 0, kReleasePipe = 1, kConnectGlobalUser = 2, kReleaseUser = 4, kGenericInterface = 12 };

// A message waiting on a pipe; `data` stays valid until Steam_FreeLastCallback.
struct CallbackMessage {
  std::int32_t user;
  std::int32_t id;
  unsigned char* data;
  std::int32_t size;
};

// The message that says an asynchronous call has finished; its result is fetched with Steam_GetAPICallResult.
inline constexpr std::int32_t kCallCompleted = 703;
struct CallCompleted {
  std::uint64_t call;
  std::int32_t callback;
  std::uint32_t size;
};

using GetCallbackFn = bool (*)(std::int32_t pipe, CallbackMessage* message, std::int32_t* call);
using FreeCallbackFn = bool (*)(std::int32_t pipe);
using GetCallResultFn = bool (*)(std::int32_t pipe, std::uint64_t call, void* out, int size, int expected, bool* failed);
using CreateInterfaceFn = void* (*)(const char* version, int* status);

template <typename Result, typename... Args>
Result call_slot(void* object, int slot, Args... args) {
  const auto table = *static_cast<Result (***)(void*, Args...)>(object);
  return table[slot](object, args...);
}

// The running client's library path, from the registry; false when Steam is not running.
bool client_library(std::wstring& path);
// Makes this process the Radical Dreamers Edition for the client (the app id it reports and owns).
void set_app_id();

}  // namespace oc::platform::steam_abi
#endif
