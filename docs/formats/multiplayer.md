# Multiplayer presence and chat

OpenCross's optional multiplayer shows other players' parties walking in the same field room and carries room and global chat. It is an OpenCross feature, not an RDE one: off by default (`[Multiplayer] Enabled`, `config.md`), and purely cosmetic. Remote parties are never entities of the room: they run no scripts, touch no story variable, flag, save or RNG, collide with nothing and cannot be talked to. Runs with `--config` and `OC_QUIET` runs never connect.

There is one server implementation, the Cloudflare Worker in `server/cloudflare/`. The public deployment is the Master server, `wss://opencross-multiplayer.sphy.workers.dev/ws`, fixed in the client. Anyone can run the same Worker on their own machine with `npx wrangler dev`, or deploy it to their own Cloudflare account, and choose "Local / other" with that address (`[Multiplayer] Server=other`, `ServerUrl`; section 6).

## 1. Transport

One WebSocket per client, `wss://` (Master, or another server online) or `ws://` (a server on this machine), path `/ws`. Every message is one UTF-8 JSON text frame holding an object whose `t` member names the message. Unknown members are ignored and unknown `t` values are dropped, so either side may add members without breaking the other. Numbers are integers unless stated.

The client connects when multiplayer is enabled and reconnects after a drop with a back-off of 2, 4, 8, 16, then every 30 s; a `welcome` starts the back-off over. The server closes a connection that sends nothing for 60 s; the client sends `ping` after 20 s without sending anything, and drops and remakes a connection that has received nothing for 75 s. After `error` `banned` or `version` it stops reconnecting until multiplayer is switched off and on, or the server address changes.

## 2. Identity

Each install makes a random 128-bit player id on first use and keeps it in `[Multiplayer] PlayerId` (32 lower-case hex digits). The client sends it only in `hello`. The server never repeats it: other clients know a player by `pid`, the first 16 hex digits of the SHA-256 of the id's 32 hex characters, so knowing someone's `pid` does not let a client claim their id.

Names are labels, not identities. Nothing proves a Steam name, and two players may show the same one. The name a client sends is, in order: `[Multiplayer] Name` when set, else the Steam display name (from the Steam connection when `[Game] Steam` is on, else the one Steam stored on this machine for the current user), else `Player`.

## 3. Client to server

| `t` | Members | Meaning |
|---|---|---|
| `hello` | `v` (protocol version, 1), `id`, `name`, `star`, `ver` (OpenCross version string), optional `admin` (the server's admin token, `[Multiplayer] AdminToken`; the client sends it only to the Master server) | First message. Anything before it closes the connection. |
| `profile` | `name`, `star` | The name or Star Level changed. |
| `room` | `room` | The field room the player is in (the room file name, such as `alu_a01f`), or `""` when not in a field room (title, world map, movie, ending). Sent after `welcome` and on every change. |
| `party` | `models` | The party's field model per slot, leader first: up to 3 integers, the model byte each party actor shows (the filled slots up to the first empty one). Sent after `welcome` and whenever it changes. |
| `pres` | `x`, `y`, `z`, `f`, `c`, `m`, `s`, `seq` | The leader's state (section 5). |
| `chat` | `scope` (`room` or `global`), `text` | A chat line. |
| `ping` | | Keep-alive. |

## 4. Server to client

| `t` | Members | Meaning |
|---|---|---|
| `welcome` | `pid`, `admin` (true when `hello` carried the right admin token), `limits` (`chat_len`, `name_len`, `pres_hz`, `chat_per_10s`) | `hello` accepted. |
| `peers` | `room`, `list` | Answer to `room`: everyone already in it. Each entry has `pid`, `name`, `star`, `models`, and `pres` (the last `pres` object, or `null`). |
| `join` | `pid`, `name`, `star`, `models`, `pres` | A player entered your room. |
| `leave` | `pid` | A player left your room or disconnected. |
| `profile` | `pid`, `name`, `star` | A player in your room changed name or Star Level. |
| `party` | `pid`, `models` | A player in your room changed party. |
| `pres` | `pid` and the `pres` members | A player in your room moved. |
| `occupancy` | `others` | How many other players are in your room; sent with `peers` and on every `join` / `leave`. |
| `chat` | `scope`, `pid`, `name`, `text`, `ts` (Unix ms), `admin` (present and true when the sender's `hello` carried the right admin token) | A chat line; `room` lines only reach players in the sender's room at the time. |
| `history` | `scope`, `lines` | The last 20 lines of global chat after `welcome`, and of the room after `peers`, oldest first, each shaped like `chat`. |
| `error` | `code`, `msg` | `code`: `rate` (message dropped by a rate limit), `bad` (malformed), `version` (protocol too old: the client shows `msg` and stops reconnecting), `banned` (the connection is then closed and the client stops reconnecting). |
| `pong` | | Answer to `ping`. |

The server trims `name` to `name_len` (24) code points and `text` to `chat_len` (200), strips control characters, and drops an empty result. Chat is plain text: the client never interprets links or markup.

## 5. Presence

`pres` describes only the party leader, in the field's own units: `x`, `y`, `z` the position in world units (rounded), `f` the facing (4096 = one turn), `c` the field clip the leader plays (`game::FieldClip`), `m` 1 while it moves, and `s` the player's state:

| `s` | State | How others draw the party |
|---|---|---|
| 0 | In the field, free to move | Walking. |
| 1 | In a battle | Standing still, with crossed swords by the name. |
| 2 | In a menu, shop or the pause screen | Standing still, with a menu icon by the name. |
| 3 | In an event (a script holds the controls, dialogue is open, a fade or a room change runs) | Standing still. |
| 4 | Typing in the chat (its input line is open in free play) | Standing still, with a speech-bubble icon by the name. |

`seq` counts up by one per message, so a receiver drops one that arrives out of order.

To save traffic the client sends `pres` only while `occupancy` says someone else is in the room, at most 10 times a second, and only when a member other than `seq` has changed; a stop (the last message with `m` 0) is always sent. The server keeps each player's last `pres` in the current room and hands it to whoever joins the room, so a player who stands still is still drawn; leaving a room forgets it, so a `join` never carries the previous room's spot. A client reports its room only once its leader stands in it, so its first pose is a real spot of that room.

The local side reports the party leader (the entity of party slot 0) as it stands after the field tick: its position rounded to whole units, its facing, `c` the clip id it plays when that is a field clip below 0x80 (else 1, standing), `m` while it moved on the tick. `s` is what the field can tell: 1 while a battle is being set up, 2 while a script screen (main menu, shop, name entry, save) is requested or open, 3 while a movie plays or the controls are not free (a script holds them, a message window is open, a fade or a room change runs), else 0. The pad's own main menu and the pause screen are the host's to report. `party` sends the model byte each slot's entity shows (a disguise shows), or the character record's model byte for a slot whose entity is not bound.

Each client works out the rest itself, on its own field ticks (`game::RemoteParties`, `src/game/remote_parties.*`):

- **Leader.** Every received pose is stamped with the local tick it arrived on. The leader is shown 9 ticks (150 ms) late, interpolated between the two poses around that moment, the facing the short way round; past the newest pose it holds there (no extrapolation). A pose that arrives after a stop (the one before had `m` 0, and more than 9 ticks have passed) gets the previous pose repeated 9 ticks before it, so the move plays over the delay instead of jumping. A pose more than 400 units from the one before (a room entry, a teleport) is shown at once and the followers regroup behind it. The height is the sender's.
- **Followers.** A follower is placed in the party V behind the remote leader (128 units away, an eighth of a turn to either side of straight behind, facing the leader's way: the formation the field gives a party on room entry), at the half V or on the leader's spot when the V spot has no floor within 64 units of the leader's height, on its first tick with a model and after each arrival or snap. After that it moves by the field's follow rules (`rules/field_actors.md` section 4.2) over rings of the remote members' own paths: slot 1 follows the leader over the leader's path, slot 2 follows slot 1 over slot 1's path (each path a point per move of more than half a unit, at most 0x80 points). Nearer than 80 units to the leader it steps 5 units straight away from it, facing it; within 200 units of the member it follows it stands; else it heads for the newest point of the path within 50 units of it (the newest point when there is none, or when the one found is within the step) at the follower speed: (0x50 - 16 per slot) / 16 units a tick walking, plus 0x48 running past 230 units, 0xF0 from 351. It takes the walk-mesh height under its spot when that floor is within 64 units of the path's height, else the path's.
- **Arrival.** A party that arrives, or jumps (a snap), is drawn once its spot has held 30 ticks (0.5 s) without another jump and its player has had the controls since (`s` 0, 2 or 4), or after 300 ticks (5 s) of an event: a room's entrance moves and scripted walk-in play out unseen and the party appears where it ends up, while a player standing through a long scene still shows.
- **Size.** Every member takes the local leader's model scale (the room's scripts size the party), each by its own model's field size.
- **Animation.** The leader plays the walk clip, or the run clip when its clip is the run clip, while it moves on the tick or its shown pose has `m` 1 (a late pose leaves it waiting a few ticks; the walk cycle runs on instead of restarting). A follower walks or runs as its follow rule says. A member standing plays the stand clip with the party actors' idle timers. In a menu or an event (`s` 2, 3, 4) the leader stands. Each member's light eases toward the zone colour of its triangle and its blob shadow toward the party actors' strength (0 on water and stairs), as a party actor's do.
- **Drawing.** Members are drawn as party actors are (the same mesh pipeline state, art mode, HD textures, `[Shaders]` character shading and blob shadows; the `[Enhancements] Lighting` silhouette shadows are not cast for them), never as room entities: no script, no collision, no trigger, no talk, no culling. A party in a battle (`s` 1) stands where it was. With the `[Multiplayer]` options: none with `ShowPlayers` off; at most `MaxParties`, those whose leaders are nearest the camera's target (the entity the scroll follows) on the ground plane; with `Translucent` each member at half opacity (only its nearest surface shows) and its shadow at half strength.
- **Models.** A model byte is loaded through the field's character-model cache on a worker thread; nothing waits for it. A party is drawn once its leader's model is ready; a follower whose model is not ready yet, or never will be, is left out, and a leader that cannot be loaded hides the party. A room change or an art switch places every member again; the models only remote parties used leave the cache then, unless a party still in the room shows them.
- **Labels.** After each draw the field gives a label point per drawn party: 64 units above the leader's head (attach point 1; 256 units above the feet for a model without it), projected into the window's field picture, with the party's `pid`, name and state; the host draws the name, Star Level and state icon there. The local leader gets the same point (`FieldWorld::local_label()`), for the player's own chat bubbles. The points are placed only while the host asks for them (`RemoteDrawOptions::labels`, on while connected). A point outside the visible picture gives no label.

## 6. The server

`server/cloudflare/` holds the Worker (TypeScript), `wrangler.toml` and a README. All connections go to one Durable Object that keeps the rooms, presence and chat history in memory, using the WebSocket hibernation API. It stores only the ban list.

- At most 4 open connections per IP address (more are refused with HTTP 429). After 10 wrong admin tokens from one address in 10 minutes, through the admin endpoints or `hello`, every admin attempt from it is refused for that long (HTTP 429; a `hello` simply gets no admin mark). Admin tokens are compared as SHA-256 digests in constant time. Presence numbers are clamped to the 32-bit signed range before they are passed on.
- Rate limits, per connection: `pres` 15 a second (more are dropped silently), `chat` 5 in any 10 s (more get `error` `rate`), other messages 10 a second.
- Bans: by player id and by IP address, kept in the Durable Object's storage. The admin endpoint `POST /admin/ban` and `POST /admin/unban` take `{"pid": ...}` or `{"ip": ...}` and need `Authorization: Bearer <ADMIN_TOKEN>` (a Worker secret); `GET /admin/players` lists the connected players with their `pid`, name, room and IP.
- On this machine: `cd server/cloudflare; npm install; npx wrangler dev` serves `ws://127.0.0.1:8787/ws`, the default `ServerUrl`. Deploying: `npx wrangler deploy` from the operator's own Cloudflare account.

## 7. Client side

The client trusts no server further than it needs: it keeps at most 64 remote parties per room, drops any message whose `pid` is not 1 to 32 letters and digits (a `pid` is stored in the ini's mute list), refuses JSON nested deeper than 64 or with more than 256 members in one object, handles no message from a connection it has already given up, and sends `[Multiplayer] AdminToken` only to the Master server.

Mutes are the player's own: `[Multiplayer] Muted` lists `pid`s whose chat lines are hidden, and whose parties too when `MuteHidesParty` is on. Nothing about a mute is sent to the server.

The local state `s` is the field's own (0 free, 3 in an event), raised to 1 during a field battle and to 2 under the main menu, a shop, the pause screen, the key-item list, the party and monster screens, a name entry or the settings window. A room change clears the table of remote parties, the occupancy and the room chat; the server's `peers`, `occupancy` and room `history` fill them again. A `history` replaces that scope's lines; its lines count as old, so they are not shown as new.

The chat panel sits at the bottom left, drawn with ImGui like the settings window. T opens its input line (Enter is the game's confirm key); while it is open the game gets no keys, Enter sends the line and closes it, Esc closes it without sending. A tickbox by the input line sends to `global` instead of `room`; Tab switches it while typing. Lines show as `[R] name: text` or `[G] name: text`, an admin's name in bold red, oldest first by the server's `ts`; a line refused with `error` `rate` leaves a grey notice. `[Multiplayer] Chat` decides what shows while not typing: the last 8 lines (`always`), each line for 10 s after it arrives with a fade over its last 2 s (`new`), or nothing (`hidden`). There is no on-screen keyboard; on a Steam Deck the Steam keyboard (Steam + X) types into the line. The labels over remote leaders give the name, a star and the Star Level, with a menu icon (state 2) or a speech-bubble icon (state 4, typing); they are not drawn while the local player is in a battle, a menu or a movie. With `[Multiplayer] ChatBubbles` on, a fresh `room` line (not one from `history`) also shows in a speech bubble over the speaker's party: the newest line of each player, for 4 s plus 0.05 s per byte of text (at most 9 s), fading over its last 0.6 s, wrapped at 14 font heights and cut to 120 bytes. The player's own lines, which the server echoes back, show over the local leader. Muted players get no bubble; `global` lines never do. Bubbles follow the same points as the labels and show when the labels could (`ShowLabels` off keeps the bubbles). Player text uses a font with Latin, Greek, Cyrillic, Japanese and common symbols; other characters show as `?`.
