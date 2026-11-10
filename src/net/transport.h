#pragma once
// The multiplayer client's connection: one WebSocket carrying UTF-8 text messages. connect() returns at once; the transport reports
// through poll() that it opened, each message received, and that it closed (with a reason when it failed). The real one runs on the
// WinHTTP WebSocket API; tests supply their own.
#include <memory>
#include <string>

namespace oc::net {

struct TransportEvent {
  enum class Kind { Opened, Message, Closed };
  Kind kind = Kind::Message;
  // Message: the text. Closed: why, for the status line (empty for a close the client asked for or a clean close by the server).
  std::string text;
};

class Transport {
 public:
  virtual ~Transport() = default;
  // Starts connecting to a ws:// or wss:// URL, dropping any earlier connection.
  virtual void connect(const std::string& url) = 0;
  // Queues one text message; dropped when not open.
  virtual void send(std::string text) = 0;
  // The next event; false when none is waiting.
  virtual bool poll(TransportEvent& event) = 0;
  // Closes the connection. No Closed event follows a close() the client asked for.
  virtual void close() = 0;
};

// The WinHTTP WebSocket transport (system proxy settings, TLS for wss://).
std::unique_ptr<Transport> make_websocket_transport(const std::string& user_agent);

}  // namespace oc::net
