# OpenCross multiplayer server

A Cloudflare Worker with one Durable Object. It implements the protocol in `docs/formats/multiplayer.md`: rooms, presence relay, room, global and private chat, battle challenges up to the battle's start, rate limits and bans.

## Run locally

```
cd server/cloudflare
npm install
npx wrangler dev
```

The server listens on `ws://127.0.0.1:8787/ws`. In the game's Multiplayer tab choose Server "Local / other"; its default address is this one. Admin endpoints are disabled (503) until an `ADMIN_TOKEN` exists; for local use create `.dev.vars` (ignored by git) containing `ADMIN_TOKEN=some-long-random-string`.

Checks: `npx tsc --noEmit` type-checks; `powershell -File tests/multiplayer/run.ps1` starts a throwaway server and runs `test/protocol.mjs` against it (needs Node 22 or later; skips with a message when Node is missing).

## Deploy from your own account

```
cd server/cloudflare
npm install
npx wrangler login
npx wrangler secret put ADMIN_TOKEN
npx wrangler deploy
```

The Worker is then at `wss://opencross-multiplayer.<your-subdomain>.workers.dev/ws`. `wrangler.toml` declares the Durable Object with a `new_sqlite_classes` migration, which is what the free plan requires.

## Admin endpoints

All need `Authorization: Bearer <ADMIN_TOKEN>`; they answer 401 for a wrong or missing token and 503 when no token is configured. A player is known by `pid` (as other clients see it).

```
curl -H "Authorization: Bearer $TOKEN" https://HOST/admin/players
curl -X POST -H "Authorization: Bearer $TOKEN" -d '{"pid":"0123456789abcdef"}' https://HOST/admin/ban
curl -X POST -H "Authorization: Bearer $TOKEN" -d '{"ip":"203.0.113.9"}' https://HOST/admin/ban
curl -X POST -H "Authorization: Bearer $TOKEN" -d '{"pid":"0123456789abcdef"}' https://HOST/admin/unban
```

`players` lists `pid`, `name`, `room` and `ip` of every connected player. Banning disconnects matching connections with `error` `banned` and refuses them afterwards. A connection from a banned IP is told on its first message. The ban list lives in the Durable Object's storage and survives restarts.

## Notes for the free plan

- Everything goes through a single Durable Object named `world`.
- Connections use the WebSocket hibernation API, so an idle object is not billed for duration while sockets stay open. Per-connection state is kept in the socket attachment and rebuilt after a wake-up.
- No chat is stored: lines are relayed to whoever is connected at the time, and nothing is replayed to newcomers.
- A pending challenge (with its party card) and a player's battle live in the socket attachments and survive hibernation. The 10 s cooldown between two challenges of the same pair is memory only; a hibernation forgets it.
- A 30 second alarm sweeps connections that have been silent for 60 s, and fires early when a challenge expires. The alarm stops once no socket is left.
- Cloudflare bills incoming WebSocket messages at a fixed ratio to requests, and limits requests and Durable Object usage per day. Check Cloudflare's current pricing and limits before opening the server to many players.
- `IDLE_MS` (a Worker variable) overrides the 60 s idle limit, `CHALLENGE_MS` the 30 s a challenge waits and `AWAY_MS` the 30 s a battle member who disconnected has to come back; the tests use them to keep the checks short. `PVP_OPENING_STAMINA` sets `opening_stamina` in `pvp_start` (tenths, default 35, 0..70).
- The versus battle link (`docs/formats/multiplayer.md` section 8.4) relays each game's side, commands and state checks to the other in order; a member who disconnects is kept in the object's storage (`away:<pid>`) until it resumes, forfeits or the battle ends. `tests/multiplayer/run.ps1 -Battles N` plays whole battles over a local server with `opencross ocbattle versus-net`.
