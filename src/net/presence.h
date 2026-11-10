#pragma once
// The presence records passed between the multiplayer client (net) and the field (game): what the local player sends about their
// party, and what is known about each remote party in the same room. Plain data, no dependencies.
#include <array>
#include <cstdint>
#include <string>

namespace oc::net {

// The local or remote player's activity (the `s` member of a `pres` message).
enum class PresenceState : int { Field = 0, Battle = 1, Menu = 2, Event = 3, Typing = 4 };

// The party leader's state as one `pres` message carries it. Position in world units, facing 4096 = one turn, clip a game::FieldClip.
struct LeaderPose {
  int x = 0, y = 0, z = 0;
  int facing = 0;
  int clip = 0;
  bool moving = false;
  PresenceState state = PresenceState::Field;
  bool operator==(const LeaderPose&) const = default;
};

// The field model byte of each party slot, leader first; -1 = empty slot.
using PartyModels = std::array<int, 3>;

// What the field reports about the local player once per tick. `room` is empty outside a field room.
struct LocalPresence {
  std::string room;
  PartyModels models{-1, -1, -1};
  LeaderPose pose;
  int star = 0;
};

// One remote party in the local player's room. `pose_seq` rises each time a new pose arrives; `has_pose` is false until the first.
struct RemoteParty {
  std::string pid;
  std::string name;
  int star = 0;
  PartyModels models{-1, -1, -1};
  LeaderPose pose;
  bool has_pose = false;
  std::uint32_t pose_seq = 0;
};

// How the field draws remote parties ([Multiplayer] ShowPlayers / MaxParties / Translucent). `labels`: place the label points over the
// remote leaders and the local one, for the name labels and the chat bubbles.
struct RemoteDrawOptions {
  bool show = true;
  int max_parties = 8;
  bool translucent = false;
  bool labels = false;
};

// A remote leader's name label as the field places it this frame, in window pixels (top-centre of the label).
struct RemoteLabel {
  std::string pid;
  std::string text;
  float x = 0, y = 0;
  PresenceState state = PresenceState::Field;
};

}  // namespace oc::net
