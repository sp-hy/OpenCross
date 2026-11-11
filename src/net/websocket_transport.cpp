// The WebSocket transport on WinHTTP. A worker thread makes the connection (HTTP upgrade, TLS for wss://, the system proxy) and then
// sends the queued messages; once the socket is open a receiver thread blocks in the receive call and queues what arrives, because
// WinHTTP's synchronous socket cannot wait for a message and a send on one thread. close() aborts both by closing the WinHTTP handles.
#include "net/transport.h"

#include <condition_variable>
#include <deque>
#include <format>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#endif

namespace oc::net {

namespace {

#ifdef _WIN32

constexpr size_t kMaxMessage = 1 << 20;

std::wstring widen(std::string_view text) {
  if (text.empty()) return {};
  const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring wide(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
  return wide;
}

std::string error_text(DWORD code) {
  switch (code) {
    case ERROR_WINHTTP_TIMEOUT: return "The server did not answer in time";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
    case ERROR_WINHTTP_CANNOT_CONNECT: return "Could not reach the server";
    case ERROR_WINHTTP_CONNECTION_ERROR: return "Connection lost";
    case ERROR_WINHTTP_SECURE_FAILURE: return "Secure connection failed";
    default: return std::format("Network error {}", code);
  }
}

class WebSocketTransport final : public Transport {
 public:
  explicit WebSocketTransport(std::string user_agent) : agent_(widen(user_agent)) {}
  ~WebSocketTransport() override { close(); }

  void connect(const std::string& url) override {
    close();
    std::lock_guard lock(mutex_);
    stop_ = false;
    open_ = false;
    reported_ = false;
    receiver_done_ = false;
    worker_ = std::thread([this, url] { run(url); });
  }

  void send(std::string text) override {
    std::lock_guard lock(mutex_);
    if (!open_) return;
    outgoing_.push_back(std::move(text));
    wake_.notify_all();
  }

  bool poll(TransportEvent& event) override {
    std::lock_guard lock(mutex_);
    if (events_.empty()) return false;
    event = std::move(events_.front());
    events_.pop_front();
    return true;
  }

  void close() override {
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
      open_ = false;
      release_handles();
      wake_.notify_all();
    }
    if (worker_.joinable()) worker_.join();
    std::lock_guard lock(mutex_);
    outgoing_.clear();
    events_.clear();
  }

 private:
  // Closes every WinHTTP handle still held (the caller holds the lock); a call blocked on one of them returns with an error.
  void release_handles() {
    for (HINTERNET* handle : {&socket_, &request_, &connection_, &session_}) {
      if (*handle) WinHttpCloseHandle(*handle);
      *handle = nullptr;
    }
  }

  // Takes a new handle into `slot` unless close() has run meanwhile (then the handle is closed and false returned).
  bool adopt(HINTERNET& slot, HINTERNET handle) {
    std::lock_guard lock(mutex_);
    if (stop_) {
      WinHttpCloseHandle(handle);
      return false;
    }
    slot = handle;
    return true;
  }

  // The connection ended: one Closed event per connection, none after close().
  void report_closed(std::string reason) {
    std::lock_guard lock(mutex_);
    open_ = false;
    if (stop_ || reported_) return;
    reported_ = true;
    events_.push_back({TransportEvent::Kind::Closed, std::move(reason)});
  }

  void fail(DWORD code) { report_closed(error_text(code)); }

  void run(const std::string& url) {
    connect_and_send(url);
    {
      std::lock_guard lock(mutex_);
      if (socket_) WinHttpCloseHandle(socket_);
      socket_ = nullptr;
    }
    if (receiver_.joinable()) receiver_.join();
    std::lock_guard lock(mutex_);
    release_handles();
  }

  void connect_and_send(const std::string& url) {
    std::string http_url;
    bool secure = false;
    if (url.starts_with("wss://")) http_url = "https://" + url.substr(6), secure = true;
    else if (url.starts_with("ws://")) http_url = "http://" + url.substr(5);
    else return report_closed("Bad server address (it must start with ws:// or wss://)");
    const std::wstring wide_url = widen(http_url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &parts)) return report_closed("Bad server address");
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength + parts.dwExtraInfoLength);
    if (path.empty()) path = L"/";

#ifdef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
    HINTERNET session = WinHttpOpen(agent_.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) session = WinHttpOpen(agent_.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
#else
    HINTERNET session = WinHttpOpen(agent_.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
#endif
    if (!session) return fail(GetLastError());
    if (!adopt(session_, session)) return;
    // The socket's receive waits as long as the server is quiet; the handshake has its own limit on the request.
    WinHttpSetTimeouts(session, 10000, 10000, 15000, 0);
    HINTERNET connection = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
    if (!connection) return fail(GetLastError());
    if (!adopt(connection_, connection)) return;
    const DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) return fail(GetLastError());
    if (!adopt(request_, request)) return;
    WinHttpSetTimeouts(request, 10000, 10000, 15000, 15000);
    if (!WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) ||
        !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request, nullptr))
      return fail(GetLastError());
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                        WINHTTP_NO_HEADER_INDEX);
    if (status != 101) return report_closed(std::format("The server refused the connection (HTTP {})", status));
    HINTERNET socket = WinHttpWebSocketCompleteUpgrade(request, 0);
    if (!socket) return fail(GetLastError());
    DWORD no_limit = 0;
    WinHttpSetOption(socket, WINHTTP_OPTION_RECEIVE_TIMEOUT, &no_limit, sizeof(no_limit));
    if (!adopt(socket_, socket)) return;
    {
      std::lock_guard lock(mutex_);
      if (stop_) return;
      WinHttpCloseHandle(request_);
      request_ = nullptr;
      open_ = true;
      events_.push_back({TransportEvent::Kind::Opened, {}});
      receiver_ = std::thread([this, socket] { receive_loop(socket); });
    }
    send_loop();
  }

  void send_loop() {
    while (true) {
      std::string text;
      HINTERNET socket = nullptr;
      {
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [&] { return stop_ || receiver_done_ || !outgoing_.empty(); });
        if (stop_ || receiver_done_) return;
        text = std::move(outgoing_.front());
        outgoing_.pop_front();
        socket = socket_;
      }
      const DWORD error = WinHttpWebSocketSend(socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, text.data(), static_cast<DWORD>(text.size()));
      if (error != NO_ERROR) return fail(error);
    }
  }

  void receive_loop(HINTERNET socket) {
    std::vector<char> buffer(16384);
    std::string message;
    while (true) {
      DWORD read = 0;
      WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
      const DWORD error = WinHttpWebSocketReceive(socket, buffer.data(), static_cast<DWORD>(buffer.size()), &read, &type);
      if (error != NO_ERROR) {
        fail(error);
        break;
      }
      if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
        USHORT code = 0;
        DWORD reason_size = 0;
        char reason[124] = {};
        WinHttpWebSocketQueryCloseStatus(socket, &code, reason, sizeof(reason), &reason_size);
        const bool normal = code == WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS || code == WINHTTP_WEB_SOCKET_ENDPOINT_TERMINATED_CLOSE_STATUS;
        report_closed(normal ? std::string() : std::format("The server closed the connection ({} {})", code, std::string_view(reason, reason_size)));
        break;
      }
      message.append(buffer.data(), read);
      if (message.size() > kMaxMessage) {
        report_closed("The server sent a message that is too large");
        break;
      }
      const bool whole = type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
      if (!whole) continue;
      if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
        std::lock_guard lock(mutex_);
        if (!stop_) events_.push_back({TransportEvent::Kind::Message, std::move(message)});
      }
      message.clear();
    }
    std::lock_guard lock(mutex_);
    receiver_done_ = true;
    wake_.notify_all();
  }

  std::wstring agent_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<std::string> outgoing_;
  std::deque<TransportEvent> events_;
  std::thread worker_, receiver_;
  HINTERNET session_ = nullptr, connection_ = nullptr, request_ = nullptr, socket_ = nullptr;
  // stop_: close() ran. open_: the socket is up and takes messages. reported_: this connection's Closed event is out.
  bool stop_ = false, open_ = false, reported_ = false, receiver_done_ = false;
};

#else

class WebSocketTransport final : public Transport {
 public:
  explicit WebSocketTransport(std::string) {}
  void connect(const std::string&) override { closed_ = true; }
  void send(std::string) override {}
  bool poll(TransportEvent& event) override {
    if (!closed_) return false;
    closed_ = false;
    event = {TransportEvent::Kind::Closed, "Multiplayer is not available on this platform"};
    return true;
  }
  void close() override { closed_ = false; }

 private:
  bool closed_ = false;
};

#endif

}  // namespace

std::unique_ptr<Transport> make_websocket_transport(const std::string& user_agent) { return std::make_unique<WebSocketTransport>(user_agent); }

}  // namespace oc::net
