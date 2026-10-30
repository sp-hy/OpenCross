#include "platform/updater.h"

#include <miniz.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <format>
#include <fstream>
#include <functional>
#include <system_error>

#include "core/log.h"
#include "core/version.h"
#include "platform/paths.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>
#include <winver.h>
#endif

namespace oc::platform {

namespace fs = std::filesystem;

namespace {

constexpr const char* kLatestApi = "https://api.github.com/repos/sp-hy/OpenCross/releases/latest";
constexpr uint64_t kMaxDownload = 512ull << 20;

std::string lower(std::string_view text) {
  std::string result(text);
  for (char& ch : result) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return result;
}
bool ends_with(std::string_view text, std::string_view tail) {
  return text.size() >= tail.size() && text.substr(text.size() - tail.size()) == tail;
}

// A minimal JSON reader: only what the releases/latest answer needs (top-level strings, the assets array). \u escapes of surrogate
// pairs become '?' (never needed here). `ok` goes false at the first malformed token and stays false.
struct Json {
  std::string_view text;
  size_t pos = 0;
  bool ok = true;

  void ws() {
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r')) ++pos;
  }
  bool eat(char expected) {
    ws();
    if (pos < text.size() && text[pos] == expected) {
      ++pos;
      return true;
    }
    return false;
  }
  static void utf8(std::string& out, unsigned cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) out += static_cast<char>(0xC0 | (cp >> 6)), out += static_cast<char>(0x80 | (cp & 0x3F));
    else out += static_cast<char>(0xE0 | (cp >> 12)), out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)), out += static_cast<char>(0x80 | (cp & 0x3F));
  }
  bool string(std::string& out) {
    out.clear();
    if (!eat('"')) return ok = false;
    while (pos < text.size()) {
      const char ch = text[pos++];
      if (ch == '"') return true;
      if (ch != '\\') {
        out += ch;
        continue;
      }
      if (pos >= text.size()) break;
      const char escaped = text[pos++];
      switch (escaped) {
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          if (pos + 4 > text.size()) return ok = false;
          unsigned cp = 0;
          for (int k = 0; k < 4; ++k) {
            const char hex = text[pos++];
            cp = cp * 16 + (hex >= '0' && hex <= '9' ? hex - '0' : hex >= 'a' && hex <= 'f' ? hex - 'a' + 10 : hex >= 'A' && hex <= 'F' ? hex - 'A' + 10 : 0);
          }
          utf8(out, cp >= 0xD800 && cp < 0xE000 ? '?' : cp);
          break;
        }
        default: out += escaped; break;
      }
    }
    return ok = false;
  }
  // A string -> its text, a number / literal -> its token; objects and arrays are skipped (out empty).
  bool scalar(std::string& out) {
    ws();
    if (pos < text.size() && text[pos] == '"') return string(out);
    out.clear();
    if (pos < text.size() && (text[pos] == '{' || text[pos] == '[')) return skip();
    while (pos < text.size() && text[pos] != ',' && text[pos] != '}' && text[pos] != ']' && !std::isspace(static_cast<unsigned char>(text[pos])))
      out += text[pos++];
    return ok = !out.empty();
  }
  bool skip() {
    ws();
    if (pos >= text.size()) return ok = false;
    if (text[pos] == '"') {
      std::string discarded;
      return string(discarded);
    }
    if (text[pos] == '{' || text[pos] == '[') {
      const char close = text[pos] == '{' ? '}' : ']';
      const bool is_object = text[pos] == '{';
      ++pos;
      if (eat(close)) return true;
      do {
        if (is_object) {
          std::string key;
          if (!string(key) || !eat(':')) return ok = false;
        }
        if (!skip()) return false;
      } while (eat(','));
      return eat(close) || (ok = false);
    }
    std::string discarded;
    return scalar(discarded);
  }
  // Calls on_member(key) for each member of an object; on_member must consume the value.
  bool object(const std::function<bool(const std::string&)>& on_member) {
    if (!eat('{')) return ok = false;
    if (eat('}')) return true;
    do {
      std::string key;
      if (!string(key) || !eat(':') || !on_member(key)) return ok = false;
    } while (eat(','));
    return eat('}') || (ok = false);
  }
};

#ifdef _WIN32
std::wstring widen(std::string_view text) {
  if (text.empty()) return {};
  const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring wide(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
  return wide;
}

struct Handle {
  HINTERNET handle = nullptr;
  ~Handle() {
    if (handle) WinHttpCloseHandle(handle);
  }
};

std::string winhttp_error(DWORD code) {
  switch (code) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
    case ERROR_WINHTTP_CANNOT_CONNECT: return "Could not reach GitHub (no internet connection?)";
    case ERROR_WINHTTP_TIMEOUT: return "GitHub did not answer in time";
    case ERROR_WINHTTP_SECURE_FAILURE: return "Secure connection to GitHub failed";
    default: return std::format("Network error {}", code);
  }
}

// `error` is set when the request itself failed (then `status` / `body` are meaningless).
struct HttpResult {
  int status = 0;
  std::string body;
  std::string error;
};

// HTTPS GET (redirects followed, system proxy). progress(received, total or 0) after each chunk.
HttpResult http_get(const std::string& url, const wchar_t* headers, const std::atomic<bool>& cancel,
                    const std::function<void(uint64_t, uint64_t)>& progress = {}) {
  HttpResult result;
  const std::wstring wurl = widen(url);
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof(uc);
  uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) {
    result.error = "Bad download address";
    return result;
  }
  const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
  const std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength + uc.dwExtraInfoLength);
  const std::wstring agent = widen(std::string("OpenCross/") + OC_VERSION_STRING);
#ifdef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
  const DWORD access = WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;
#else
  const DWORD access = WINHTTP_ACCESS_TYPE_DEFAULT_PROXY;
#endif
  Handle session{WinHttpOpen(agent.c_str(), access, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
  if (!session.handle) session.handle = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session.handle) {
    result.error = winhttp_error(GetLastError());
    return result;
  }
  WinHttpSetTimeouts(session.handle, 10000, 10000, 15000, 30000);
  Handle conn{WinHttpConnect(session.handle, host.c_str(), uc.nPort, 0)};
  Handle req{conn.handle ? WinHttpOpenRequest(conn.handle, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                         : nullptr};
  if (!req.handle || !WinHttpSendRequest(req.handle, headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(req.handle, nullptr)) {
    result.error = winhttp_error(GetLastError());
    return result;
  }
  DWORD code = 0, len = sizeof(code);
  WinHttpQueryHeaders(req.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
  result.status = static_cast<int>(code);
  DWORD total = 0;
  len = sizeof(total);
  if (!WinHttpQueryHeaders(req.handle, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &total, &len,
                           WINHTTP_NO_HEADER_INDEX))
    total = 0;
  if (total) result.body.reserve(total);
  std::vector<char> buf(64 * 1024);
  for (;;) {
    if (cancel) {
      result.error = "Cancelled";
      return result;
    }
    DWORD got = 0;
    if (!WinHttpReadData(req.handle, buf.data(), static_cast<DWORD>(buf.size()), &got)) {
      result.error = winhttp_error(GetLastError());
      return result;
    }
    if (got == 0) break;
    result.body.append(buf.data(), got);
    if (result.body.size() > kMaxDownload) {
      result.error = "Download too large";
      return result;
    }
    if (progress) progress(result.body.size(), total);
  }
  return result;
}

// The Product Version in an exe's version resource (src/opencross.rc.in), e.g. {0, 1, 0, 0}; empty when it has none.
std::vector<int> exe_file_version(const fs::path& exe) {
  DWORD dummy = 0;
  const DWORD size = GetFileVersionInfoSizeW(exe.c_str(), &dummy);
  if (!size) return {};
  std::vector<unsigned char> buf(size);
  VS_FIXEDFILEINFO* info = nullptr;
  UINT len = 0;
  if (!GetFileVersionInfoW(exe.c_str(), 0, size, buf.data()) || !VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info)
    return {};
  return {static_cast<int>(HIWORD(info->dwProductVersionMS)), static_cast<int>(LOWORD(info->dwProductVersionMS)),
          static_cast<int>(HIWORD(info->dwProductVersionLS)), static_cast<int>(LOWORD(info->dwProductVersionLS))};
}

std::string sha256_hex(const std::string& data) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  unsigned char out[32] = {};
  std::string hex;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
      if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), static_cast<ULONG>(data.size()), 0) == 0 &&
          BCryptFinishHash(hash, out, sizeof(out), 0) == 0) {
        static const char* kDigits = "0123456789abcdef";
        for (unsigned char byte : out) hex += kDigits[byte >> 4], hex += kDigits[byte & 15];
      }
      BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
  }
  return hex;
}
#endif

bool looks_like_exe(const std::string& data) {
  if (data.size() < 0x200 || data[0] != 'M' || data[1] != 'Z') return false;
  uint32_t pe = 0;
  std::memcpy(&pe, data.data() + 0x3C, 4);
  return pe + 4 <= data.size() && std::memcmp(data.data() + pe, "PE\0\0", 4) == 0;
}

// opencross.exe at the root of the zip or one folder deep.
bool exe_from_zip(const std::string& zip, std::string& out) {
  mz_zip_archive archive{};
  if (!mz_zip_reader_init_mem(&archive, zip.data(), zip.size(), 0)) return false;
  bool found = false;
  const mz_uint count = mz_zip_reader_get_num_files(&archive);
  for (mz_uint i = 0; i < count && !found; ++i) {
    char name[512] = {};
    mz_zip_reader_get_filename(&archive, i, name, sizeof(name));
    std::string lowered = lower(name);
    std::replace(lowered.begin(), lowered.end(), '\\', '/');
    if (!ends_with(lowered, "opencross.exe")) continue;
    const std::string dir = lowered.substr(0, lowered.size() - std::strlen("opencross.exe"));
    if (!dir.empty() && (dir.back() != '/' || dir.find('/') != dir.size() - 1)) continue;
    size_t size = 0;
    if (void* data = mz_zip_reader_extract_to_heap(&archive, i, &size, 0)) {
      out.assign(static_cast<const char*>(data), size);
      mz_free(data);
      found = true;
    }
  }
  mz_zip_reader_end(&archive);
  return found;
}

}  // namespace

std::optional<std::vector<int>> parse_version(std::string_view text) {
  if (!text.empty() && (text[0] == 'v' || text[0] == 'V')) text.remove_prefix(1);
  std::vector<int> parts;
  size_t i = 0;
  while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
    int number = 0;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) number = std::min(number * 10 + (text[i++] - '0'), 1000000);
    parts.push_back(number);
    if (i + 1 < text.size() && text[i] == '.' && std::isdigit(static_cast<unsigned char>(text[i + 1]))) ++i;
    else break;
  }
  if (parts.empty()) return std::nullopt;
  return parts;
}

int compare_versions(const std::vector<int>& lhs, const std::vector<int>& rhs) {
  for (size_t i = 0; i < std::max(lhs.size(), rhs.size()); ++i) {
    const int lhs_part = i < lhs.size() ? lhs[i] : 0, rhs_part = i < rhs.size() ? rhs[i] : 0;
    if (lhs_part != rhs_part) return lhs_part < rhs_part ? -1 : 1;
  }
  return 0;
}

bool parse_release_json(std::string_view json, ReleaseInfo& out) {
  out = {};
  Json reader{json};
  reader.object([&](const std::string& key) {
    if (key == "tag_name") return reader.scalar(out.tag);
    if (key == "name") return reader.scalar(out.name);
    if (key == "html_url") return reader.scalar(out.html_url);
    if (key == "published_at") {
      if (!reader.scalar(out.published)) return false;
      out.published = out.published.substr(0, 10);
      return true;
    }
    if (key != "assets") return reader.skip();
    if (!reader.eat('[')) return reader.skip();
    if (reader.eat(']')) return true;
    do {
      ReleaseAsset asset;
      std::string value;
      if (!reader.object([&](const std::string& asset_key) {
            if (asset_key == "name") return reader.scalar(asset.name);
            if (asset_key == "browser_download_url") return reader.scalar(asset.url);
            if (asset_key == "size") {
              if (!reader.scalar(value)) return false;
              asset.size = std::strtoull(value.c_str(), nullptr, 10);
              return true;
            }
            if (asset_key == "digest") {
              if (!reader.scalar(value)) return false;
              if (value.rfind("sha256:", 0) == 0) asset.sha256 = lower(value.substr(7));
              return true;
            }
            return reader.skip();
          }))
        return false;
      out.assets.push_back(std::move(asset));
    } while (reader.eat(','));
    return reader.eat(']');
  });
  return reader.ok && !out.tag.empty() && out.tag != "null";
}

const ReleaseAsset* pick_asset(const ReleaseInfo& release) {
  for (const ReleaseAsset& asset : release.assets)
    if (lower(asset.name) == "opencross.exe") return &asset;
  for (const ReleaseAsset& asset : release.assets)
    if (const std::string lowered = lower(asset.name); lowered.rfind("opencross", 0) == 0 && ends_with(lowered, ".zip")) return &asset;
  const ReleaseAsset* only = nullptr;
  int count = 0;
  for (const ReleaseAsset& asset : release.assets)
    if (const std::string lowered = lower(asset.name); ends_with(lowered, ".exe") || ends_with(lowered, ".zip")) only = &asset, ++count;
  return count == 1 ? only : nullptr;
}

Updater::Updater() = default;

Updater::~Updater() {
  cancel_ = true;
  if (worker_.joinable()) worker_.join();
}

Updater::Status Updater::status() const {
  std::lock_guard lock(mu_);
  return st_;
}

bool Updater::busy() const {
  std::lock_guard lock(mu_);
  return st_.state == State::Checking || st_.state == State::Downloading;
}

// A finished worker's last step was the final state under this lock, so joining here never blocks.
void Updater::check() {
  std::lock_guard lock(mu_);
  if (st_.state == State::Checking || st_.state == State::Downloading) return;
  if (worker_.joinable()) worker_.join();
  const std::string installed = st_.state == State::Installed ? st_.latest : installed_;
  st_ = {};
  st_.state = State::Checking;
  installed_ = installed;
  worker_ = std::thread([this] { run(false); });
}

void Updater::download() {
  std::lock_guard lock(mu_);
  if (st_.state != State::Available || !asset_) return;
  if (worker_.joinable()) worker_.join();
  st_.state = State::Downloading;
  st_.progress = 0.0f;
  worker_ = std::thread([this] { run(true); });
}

void Updater::set_failed(std::string msg) {
  log::warn("update: {}", msg);
  std::lock_guard lock(mu_);
  st_.state = State::Failed;
  st_.message = std::move(msg);
}

// The worker: `install` false = the release check, true = the download + swap. The downloaded exe's own version resource must be
// newer than this build (a tag bumped without rebuilding would otherwise be offered again forever). The swap renames the running
// exe to <exe>.old (a running exe can be renamed but not overwritten) and the download to <exe>; a failed second rename puts the
// running one back.
void Updater::run(bool install) {
#ifdef _WIN32
  if (!install) {
    const HttpResult response = http_get(kLatestApi, L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28", cancel_);
    if (!response.error.empty()) return set_failed(response.error);
    if (response.status == 404) return set_failed("No release has been published yet");
    if (response.status == 403 || response.status == 429) return set_failed("GitHub is limiting requests; try again in a while");
    if (response.status != 200) return set_failed(std::format("GitHub answered HTTP {}", response.status));
    ReleaseInfo rel;
    if (!parse_release_json(response.body, rel)) return set_failed("Could not read GitHub's answer");
    const auto latest = parse_version(rel.tag), current = parse_version(OC_VERSION_STRING);
    if (!latest || !current) return set_failed(std::format("Unknown release tag \"{}\"", rel.tag));
    const ReleaseAsset* asset = pick_asset(rel);
    log::info("update: current {}, latest {} ({} assets, pick {})", OC_VERSION_STRING, rel.tag, rel.assets.size(), asset ? asset->name : "none");
    std::lock_guard lock(mu_);
    st_.latest = rel.tag;
    st_.html_url = rel.html_url.empty() ? kReleasesUrl : rel.html_url;
    st_.published = rel.published;
    if (compare_versions(*latest, *current) <= 0) st_.state = State::UpToDate;
    else if (!installed_.empty() && parse_version(installed_) && compare_versions(*latest, *parse_version(installed_)) <= 0) st_.state = State::Installed;
    else if (!asset) st_.state = State::Failed, st_.message = std::format("{} has no opencross.exe or .zip to download", rel.tag);
    else st_.state = State::Available, asset_ = *asset;
    return;
  }

  ReleaseAsset asset;
  {
    std::lock_guard lock(mu_);
    asset = *asset_;
  }
  const HttpResult response = http_get(asset.url, L"Accept: application/octet-stream", cancel_, [&](uint64_t got, uint64_t total) {
    if (!total) total = asset.size;
    std::lock_guard lock(mu_);
    st_.progress = total ? std::min(1.0f, static_cast<float>(static_cast<double>(got) / static_cast<double>(total))) : 0.0f;
  });
  if (!response.error.empty()) return set_failed(response.error);
  if (response.status != 200) return set_failed(std::format("Download failed (HTTP {})", response.status));
  if (asset.size && response.body.size() != asset.size) return set_failed("Download incomplete; try again");
  if (!asset.sha256.empty() && sha256_hex(response.body) != asset.sha256) return set_failed("Download damaged (SHA-256 mismatch); try again");
  std::string exe_bytes;
  if (ends_with(lower(asset.name), ".zip")) {
    if (!exe_from_zip(response.body, exe_bytes)) return set_failed(std::format("{} holds no opencross.exe", asset.name));
  } else {
    exe_bytes = response.body;
  }
  if (!looks_like_exe(exe_bytes)) return set_failed("The download is not a Windows program");

  const fs::path exe = exe_path();
  if (exe.empty()) return set_failed("Cannot find the running exe");
  const fs::path fresh = fs::path(exe).concat(".new"), old = fs::path(exe).concat(".old");
  {
    std::ofstream out(fresh, std::ios::binary | std::ios::trunc);
    out.write(exe_bytes.data(), static_cast<std::streamsize>(exe_bytes.size()));
    if (!out.flush()) {
      std::error_code ec;
      out.close();
      fs::remove(fresh, ec);
      return set_failed("Cannot write to the game folder; download it from GitHub by hand instead");
    }
  }
  std::error_code ec;
  {
    const std::vector<int> got = exe_file_version(fresh);
    if (got.empty() || compare_versions(got, *parse_version(OC_VERSION_STRING)) <= 0) {
      fs::remove(fresh, ec);
      const std::string version_text = got.empty() ? "no version" : std::format("version {}.{}.{}", got[0], got[1], got[2]);
      std::string tag;
      {
        std::lock_guard lock(mu_);
        tag = st_.latest;
      }
      return set_failed(std::format("The exe in release {} reports {}, not newer than this one (v{}); nothing was changed", tag, version_text,
                                    OC_VERSION_STRING));
    }
  }
  fs::remove(old, ec);
  if (!MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    const DWORD error = GetLastError();
    fs::remove(fresh, ec);
    return set_failed(std::format("Cannot replace the exe (error {}); download it from GitHub by hand instead", error));
  }
  if (!MoveFileExW(fresh.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    const DWORD error = GetLastError();
    MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);
    fs::remove(fresh, ec);
    return set_failed(std::format("Cannot replace the exe (error {}); download it from GitHub by hand instead", error));
  }
  log::info("update: installed {} over {} ({} bytes); old exe kept as {}", asset.name, exe.string(), exe_bytes.size(), old.filename().string());
  std::lock_guard lock(mu_);
  st_.state = State::Installed;
  st_.progress = 1.0f;
#else
  (void)install;
  set_failed("Updating is only supported on Windows");
#endif
}

bool Updater::restart() {
#ifdef _WIN32
  const fs::path exe = exe_path();
  std::wstring cmd = GetCommandLineW();
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
    log::error("update: restart failed ({})", GetLastError());
    return false;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
#else
  return false;
#endif
}

// Deletes a <exe>.new that never got swapped in and the <exe>.old of a finished update; the latter on a detached thread with
// retries, since right after a restart the previous process may still be closing and holding its file.
void Updater::cleanup_after_update() {
  const fs::path exe = exe_path();
  if (exe.empty()) return;
  const fs::path old = fs::path(exe).concat(".old"), fresh = fs::path(exe).concat(".new");
  std::error_code ec;
  fs::remove(fresh, ec);
  if (!fs::exists(old, ec)) return;
  std::thread([old] {
    for (int i = 0; i < 40; ++i) {
      std::error_code retry_ec;
      if (fs::remove(old, retry_ec) || !fs::exists(old, retry_ec)) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
  }).detach();
}

}  // namespace oc::platform
