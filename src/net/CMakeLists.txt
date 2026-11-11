# Multiplayer presence and chat: the protocol client, its JSON and the WebSocket transport
add_library(oc_net STATIC
  presence.h
  json.h
  json.cpp
  transport.h
  websocket_transport.cpp
  multiplayer_client.h
  multiplayer_client.cpp
)
target_link_libraries(oc_net PUBLIC oc_core)
if(WIN32)
  target_link_libraries(oc_net PRIVATE winhttp bcrypt)
endif()
