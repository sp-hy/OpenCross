// XINPUT9_1_0.dll: dropped next to CHRONOCROSS_LAUNCHER.exe, it adds the OpenCross button to RDE's launcher without changing a game file.
// All four XInput exports forward to the system DLL, so the game exes, which import it as well, run exactly as they would without it.
// Inside the launcher process only (the one that has CHRONOCROSS_LAUNCHER.dll loaded) the helper's Initialize export is redirected: the
// launcher's first call to it, on its UI thread and before its window loads, runs LauncherPatch from the managed assembly embedded in
// this DLL, then carries on into the real Initialize. Any failure leaves the stock launcher.
#include <windows.h>
#include <initguid.h>
#include <metahost.h>

#pragma warning(push)
#pragma warning(disable : 4146 4278)
#import "mscorlib.tlb" raw_interfaces_only auto_rename
#pragma warning(pop)

#include "payload_id.h"

namespace {

HMODULE self_module;

using InitializeFn = LONG(__stdcall*)(HWND);
InitializeFn helper_initialize;
LONG patch_started;

struct SystemXInput {
  using GetStateFn = DWORD(WINAPI*)(DWORD, void*);
  using SetStateFn = DWORD(WINAPI*)(DWORD, void*);
  using GetCapabilitiesFn = DWORD(WINAPI*)(DWORD, DWORD, void*);
  using GetAudioGuidsFn = DWORD(WINAPI*)(DWORD, GUID*, GUID*);

  GetStateFn get_state = nullptr;
  SetStateFn set_state = nullptr;
  GetCapabilitiesFn get_capabilities = nullptr;
  GetAudioGuidsFn get_audio_guids = nullptr;

  SystemXInput() {
    wchar_t path[MAX_PATH];
    const UINT length = GetSystemDirectoryW(path, MAX_PATH);
    if (length == 0 || length + 20 > MAX_PATH) return;
    lstrcatW(path, L"\\XINPUT9_1_0.dll");
    const HMODULE module = LoadLibraryW(path);
    if (!module) return;
    get_state = reinterpret_cast<GetStateFn>(GetProcAddress(module, "XInputGetState"));
    set_state = reinterpret_cast<SetStateFn>(GetProcAddress(module, "XInputSetState"));
    get_capabilities = reinterpret_cast<GetCapabilitiesFn>(GetProcAddress(module, "XInputGetCapabilities"));
    get_audio_guids = reinterpret_cast<GetAudioGuidsFn>(GetProcAddress(module, "XInputGetDSoundAudioDeviceGuids"));
  }
};

const SystemXInput& system_xinput() {
  static const SystemXInput instance;
  return instance;
}

// A SAFEARRAY of bytes holding the embedded assembly; null when the resource is missing.
SAFEARRAY* payload_bytes() {
  const HRSRC resource = FindResourceW(self_module, MAKEINTRESOURCEW(kPayloadResource), MAKEINTRESOURCEW(10));
  if (!resource) return nullptr;
  const HGLOBAL loaded = LoadResource(self_module, resource);
  const DWORD size = SizeofResource(self_module, resource);
  if (!loaded || size == 0) return nullptr;
  SAFEARRAY* bytes = SafeArrayCreateVector(VT_UI1, 0, size);
  if (!bytes) return nullptr;
  void* data = nullptr;
  if (FAILED(SafeArrayAccessData(bytes, &data))) {
    SafeArrayDestroy(bytes);
    return nullptr;
  }
  memcpy(data, LockResource(loaded), size);
  SafeArrayUnaccessData(bytes);
  return bytes;
}

// Runs inside the runtime the launcher already started: loads the assembly into its default domain and asks for its attributes, which
// constructs LauncherPatch. The CLR and Mono (the runtime under Wine) order _Assembly differently from its ModuleResolve event on, so
// only calls from before that point, like this one, reach the same method on both.
void run_patch() {
  const HMODULE clr = GetModuleHandleW(L"mscoree.dll");
  if (!clr) return;
  const auto create_instance = reinterpret_cast<CLRCreateInstanceFnPtr>(GetProcAddress(clr, "CLRCreateInstance"));
  if (!create_instance) return;

  ICLRMetaHost* meta_host = nullptr;
  ICLRRuntimeInfo* runtime = nullptr;
  ICorRuntimeHost* host = nullptr;
  IUnknown* domain_unknown = nullptr;
  mscorlib::_AppDomain* domain = nullptr;
  mscorlib::_Assembly* assembly = nullptr;
  SAFEARRAY* bytes = nullptr;
  SAFEARRAY* attributes = nullptr;

  if (SUCCEEDED(create_instance(CLSID_CLRMetaHost, IID_ICLRMetaHost, reinterpret_cast<void**>(&meta_host))) &&
      SUCCEEDED(meta_host->GetRuntime(L"v4.0.30319", IID_ICLRRuntimeInfo, reinterpret_cast<void**>(&runtime))) &&
      SUCCEEDED(runtime->GetInterface(CLSID_CorRuntimeHost, IID_ICorRuntimeHost, reinterpret_cast<void**>(&host))) &&
      SUCCEEDED(host->GetDefaultDomain(&domain_unknown)) &&
      SUCCEEDED(domain_unknown->QueryInterface(__uuidof(mscorlib::_AppDomain), reinterpret_cast<void**>(&domain))) &&
      (bytes = payload_bytes()) != nullptr && SUCCEEDED(domain->Load_3(bytes, &assembly))) {
    assembly->GetCustomAttributes_2(VARIANT_FALSE, &attributes);
  }

  if (attributes) SafeArrayDestroy(attributes);
  if (bytes) SafeArrayDestroy(bytes);
  if (assembly) assembly->Release();
  if (domain) domain->Release();
  if (domain_unknown) domain_unknown->Release();
  if (host) host->Release();
  if (runtime) runtime->Release();
  if (meta_host) meta_host->Release();
}

// The launcher calls Initialize again after every game exits; the patch runs once. EAX is passed through whole: the export's result is
// read as a Win32 BOOL.
LONG __stdcall initialize_hook(HWND window) {
  if (InterlockedExchange(&patch_started, 1) == 0) run_patch();
  return helper_initialize(window);
}

// Points the helper's export at initialize_hook. We are one of its imports, so this runs before anyone can resolve the export; nothing
// here enters the loader or the CLR, which are off limits under the loader lock.
void redirect_helper_initialize() {
  auto* base = reinterpret_cast<BYTE*>(GetModuleHandleW(L"CHRONOCROSS_LAUNCHER.dll"));
  if (!base) return;
  const auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  const auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const IMAGE_DATA_DIRECTORY& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  if (directory.VirtualAddress == 0) return;
  const auto* exports = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(base + directory.VirtualAddress);
  const auto* names = reinterpret_cast<DWORD*>(base + exports->AddressOfNames);
  const auto* ordinals = reinterpret_cast<WORD*>(base + exports->AddressOfNameOrdinals);
  auto* functions = reinterpret_cast<DWORD*>(base + exports->AddressOfFunctions);
  for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
    if (lstrcmpA(reinterpret_cast<const char*>(base + names[i]), "_Initialize@4") != 0) continue;
    DWORD* slot = &functions[ordinals[i]];
    DWORD protection = 0;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protection)) return;
    helper_initialize = reinterpret_cast<InitializeFn>(base + *slot);
    *slot = static_cast<DWORD>(reinterpret_cast<BYTE*>(&initialize_hook) - base);
    VirtualProtect(slot, sizeof(*slot), protection, &protection);
    return;
  }
}

}  // namespace

extern "C" {

DWORD WINAPI XInputGetState(DWORD user, void* state) {
  const auto call = system_xinput().get_state;
  return call ? call(user, state) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputSetState(DWORD user, void* vibration) {
  const auto call = system_xinput().set_state;
  return call ? call(user, vibration) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetCapabilities(DWORD user, DWORD flags, void* capabilities) {
  const auto call = system_xinput().get_capabilities;
  return call ? call(user, flags, capabilities) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD user, GUID* render, GUID* capture) {
  const auto call = system_xinput().get_audio_guids;
  return call ? call(user, render, capture) : ERROR_DEVICE_NOT_CONNECTED;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
  if (reason == DLL_PROCESS_ATTACH) {
    self_module = instance;
    DisableThreadLibraryCalls(instance);
    redirect_helper_initialize();
  }
  return TRUE;
}

}  // extern "C"
