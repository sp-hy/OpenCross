// OpenCross multiplayer server: one Durable Object holds every connection, room,
// last presence and pending battles, and speaks the protocol in docs/formats/multiplayer.md.
import { DurableObject } from "cloudflare:workers";

interface Env {
  WORLD: DurableObjectNamespace;
  ADMIN_TOKEN?: string;
  IDLE_MS?: string;
  CHALLENGE_MS?: string;
  PVP_OPENING_STAMINA?: string;
  AWAY_MS?: string;
}

interface Pres {
  x: number;
  y: number;
  z: number;
  f: number;
  c: number;
  m: number;
  s: number;
  seq: number;
}

interface CardMember {
  char: number;
  name: string;
  hp: number;
  atk: number;
  def: number;
  mag: number;
  mdef: number;
  elements: number;
}

interface Card {
  star: number;
  members: CardMember[];
}

// The challenge a player has sent and that is still waiting for an answer, kept in the challenger's attachment.
interface Outgoing {
  to: string;
  expires: number;
  card: Card;
  ver: string;
  data: string;
}

interface Conn {
  banned: boolean;
  hello: boolean;
  admin: boolean;
  pid: string;
  name: string;
  star: number;
  room: string;
  models: number[];
  pres: Pres | null;
  pvp: boolean;
  out: Outgoing | null;
  battle: string;
  opponent: string;
  // The battle link: the last seq this player sent (a command or a state check), whether it sent its side, and the last relay number of
  // the battle messages sent to it.
  pvpSeq: number;
  pvpSide: boolean;
  pvpN: number;
  ip: string;
  last: number;
  presWin: number;
  presN: number;
  otherWin: number;
  otherN: number;
  battleWin: number;
  battleN: number;
  chats: number[];
}

// A battle member who disconnected: kept in storage under "away:<pid>" until it resumes, its time runs out (`forfeited`, then kept until
// `keep` so that a late resume learns it lost) or the battle ends. The link counters are the member's; `outbox` holds the battle messages
// relayed to it that it has not acknowledged.
interface Away {
  battle: string;
  opponent: string;
  deadline: number;
  seq: number;
  side: boolean;
  n: number;
  outbox: Msg[];
  forfeited: boolean;
  keep: number;
}

interface Line {
  t: "chat";
  scope: "room" | "global" | "pm";
  pid: string;
  name: string;
  text: string;
  ts: number;
  admin?: true;
  to?: string;
  to_name?: string;
}

type Msg = Record<string, unknown>;

const NAME_LEN = 24;
const CHAT_LEN = 200;
const ROOM_LEN = 64;
const MAX_FRAME = 4096;
// A pvp_side frame may be this long; every other message keeps to MAX_FRAME.
const MAX_SIDE_FRAME = 16384;
const PRES_PER_SEC = 15;
const OTHER_PER_SEC = 10;
const CHAT_PER_10S = 5;
const PRES_KEYS = ["x", "y", "z", "f", "c", "m", "s", "seq"] as const;
// Presence numbers are kept to a 32-bit signed range before they are passed on.
const PRES_LIMIT = 2147483647;
// Sockets one address may hold at once ("local", the address wrangler dev reports, is not limited).
const MAX_SOCKETS_PER_IP = 4;
// Wrong admin tokens (HTTP or hello) an address may give in AUTH_WINDOW_MS before every admin attempt from it is refused for that long.
const MAX_AUTH_FAILURES = 10;
const AUTH_WINDOW_MS = 10 * 60 * 1000;
// A challenge waits this long for its answer (CHALLENGE_MS overrides it); the same pair may not be challenged again within COOLDOWN_MS.
const CHALLENGE_MS = 30000;
const COOLDOWN_MS = 10000;
// The first side's stamina on its opening turn, tenths (PVP_OPENING_STAMINA overrides it, 0..MAX_STAMINA).
const OPENING_STAMINA = 35;
const MAX_STAMINA = 70;
const VER_LEN = 32;
const DATA_LEN = 128;
// Cloudflare refuses socket attachments over 2048 bytes; a challenge that would take the attachment past this is refused.
const MAX_ATTACHMENT = 1900;
const REFUSALS = ["declined", "busy", "timeout", "off"] as const;
const RESULTS = ["won", "lost", "aborted", "draw", "desync"] as const;
// The battle link (pvp_side, pvp_cmd, pvp_hash, pvp_resume): its own rate limit per connection, the longest side (base64 characters) and
// command text, how long a disconnected member may take to come back (AWAY_MS overrides it), how long a forfeit is remembered for a late
// resume, and how many unacknowledged messages (and characters) one member's outbox keeps.
const BATTLE_PER_SEC = 30;
const SIDE_LEN = 12288;
const CMD_LEN = 64;
const AWAY_MS = 30000;
const GONE_KEEP_MS = 10 * 60 * 1000;
const OUTBOX_MAX = 256;
const OUTBOX_CHARS = 96 * 1024;

const encoder = new TextEncoder();

function clean(text: unknown, max: number): string {
  if (typeof text !== "string") return "";
  const stripped = text.replace(/[\u0000-\u001f\u007f-\u009f]/g, "").trim();
  return Array.from(stripped).slice(0, max).join("");
}

function int(value: unknown): number | null {
  return typeof value === "number" && Number.isFinite(value) ? Math.round(value) : null;
}

function parseModels(value: unknown): number[] | null {
  if (!Array.isArray(value) || value.length > 3) return null;
  const models: number[] = [];
  for (const entry of value) {
    const model = int(entry);
    if (model === null || model < 0 || model > 255) return null;
    models.push(model);
  }
  return models;
}

function parsePres(msg: Msg): Pres | null {
  const out: Record<string, number> = {};
  for (const key of PRES_KEYS) {
    const value = int(msg[key]);
    if (value === null) return null;
    out[key] = Math.max(-PRES_LIMIT, Math.min(PRES_LIMIT, value));
  }
  return out as unknown as Pres;
}

function clamp(value: unknown, low: number, high: number): number | null {
  const number = int(value);
  return number === null ? null : Math.max(low, Math.min(high, number));
}

// A party card as a client sent it, cut to the protocol's shape; null when it is not one.
function parseCard(value: unknown): Card | null {
  if (typeof value !== "object" || value === null || Array.isArray(value)) return null;
  const card = value as Msg;
  const star = clamp(card.star, 0, 999);
  if (star === null || !Array.isArray(card.members) || card.members.length > 3) return null;
  const members: CardMember[] = [];
  for (const entry of card.members) {
    if (typeof entry !== "object" || entry === null) return null;
    const member = entry as Msg;
    const char = clamp(member.char, 0, 255);
    const hp = clamp(member.hp, 0, 9999);
    const stats = [member.atk, member.def, member.mag, member.mdef].map((stat) => clamp(stat, 0, 999));
    const elements = clamp(member.elements, 0, 99);
    if (char === null || hp === null || elements === null || stats.some((stat) => stat === null)) return null;
    const [atk, def, mag, mdef] = stats as number[];
    members.push({ char, name: clean(member.name, NAME_LEN), hp, atk, def, mag, mdef, elements });
  }
  return { star, members };
}

// The battle data fingerprint: lower-case hex, possibly empty.
function parseData(value: unknown): string | null {
  const data = typeof value === "string" ? value : "";
  return data.length <= DATA_LEN && /^[0-9a-f]*$/.test(data) ? data : null;
}

function isPid(value: unknown): value is string {
  return typeof value === "string" && /^[0-9a-f]{16}$/.test(value);
}

function isBattle(value: unknown): value is string {
  return typeof value === "string" && /^[0-9a-f]{16}$/.test(value);
}

// A seq of the battle link: 1 to 2^31 - 1.
function parseSeq(value: unknown): number | null {
  return typeof value === "number" && Number.isInteger(value) && value >= 1 && value <= PRES_LIMIT ? value : null;
}

function isHash(value: unknown): value is string {
  return typeof value === "string" && /^[0-9a-f]{16}$/.test(value);
}

// A command as the clients write it: signed decimal integers separated by single spaces.
function isCmd(value: unknown): value is string {
  return typeof value === "string" && value.length <= CMD_LEN && /^-?[0-9]+( -?[0-9]+)*$/.test(value);
}

function isSide(value: unknown): value is string {
  return typeof value === "string" && value.length > 0 && value.length <= SIDE_LEN && /^[A-Za-z0-9+/]+={0,2}$/.test(value);
}

function randomHex(bytes: number): string {
  return Array.from(crypto.getRandomValues(new Uint8Array(bytes))).map((b) => b.toString(16).padStart(2, "0")).join("");
}

async function hashId(id: string): Promise<string> {
  const digest = await crypto.subtle.digest("SHA-256", encoder.encode(id));
  const bytes = Array.from(new Uint8Array(digest).slice(0, 8));
  return bytes.map((b) => b.toString(16).padStart(2, "0")).join("");
}

// Compares the SHA-256 digests in constant time, so neither the content nor the length of the secret shows in the timing.
async function sameSecret(given: string, expected: string): Promise<boolean> {
  const [a, b] = await Promise.all([given, expected].map((text) => crypto.subtle.digest("SHA-256", encoder.encode(text))));
  return crypto.subtle.timingSafeEqual(a, b);
}

function json(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "content-type": "application/json" },
  });
}

export default {
  fetch(req: Request, env: Env): Promise<Response> | Response {
    const path = new URL(req.url).pathname;
    if (path === "/ws" || path.startsWith("/admin/")) {
      return env.WORLD.get(env.WORLD.idFromName("world")).fetch(req);
    }
    return new Response("not found", { status: 404 });
  },
};

export class World extends DurableObject<Env> {
  private cache = new WeakMap<WebSocket, Conn>();
  private authFailures = new Map<string, number[]>();
  // When each challenger last challenged each target ("challenger>target"); memory only, so a wake-up forgets the cooldowns.
  private cooldowns = new Map<string, number>();
  // Per battle member: the battle messages relayed to it that it has not acknowledged yet. Memory only while it is connected.
  private outboxes = new Map<string, Msg[]>();
  // The away records as last read or written (storage keeps them over a hibernation).
  private aways = new Map<string, Away | null>();

  // True when `ip` may try an admin token now (fewer than MAX_AUTH_FAILURES wrong ones in the last AUTH_WINDOW_MS).
  private authAllowed(ip: string): boolean {
    const now = Date.now();
    const recent = (this.authFailures.get(ip) ?? []).filter((at) => now - at < AUTH_WINDOW_MS);
    if (recent.length) this.authFailures.set(ip, recent);
    else this.authFailures.delete(ip);
    return recent.length < MAX_AUTH_FAILURES;
  }

  private authFailed(ip: string): void {
    if (this.authFailures.size > 10000) this.authFailures.clear();
    const list = this.authFailures.get(ip) ?? [];
    list.push(Date.now());
    this.authFailures.set(ip, list);
  }

  private get idleMs(): number {
    const value = Number(this.env.IDLE_MS);
    return Number.isFinite(value) && value > 0 ? value : 60000;
  }

  private get challengeMs(): number {
    const value = Number(this.env.CHALLENGE_MS);
    return Number.isFinite(value) && value > 0 ? value : CHALLENGE_MS;
  }

  private get awayMs(): number {
    const value = Number(this.env.AWAY_MS);
    return Number.isFinite(value) && value > 0 ? value : AWAY_MS;
  }

  private get openingStamina(): number {
    const value = Number(this.env.PVP_OPENING_STAMINA);
    return this.env.PVP_OPENING_STAMINA !== undefined && Number.isInteger(value) ? Math.max(0, Math.min(MAX_STAMINA, value)) : OPENING_STAMINA;
  }

  private conn(ws: WebSocket): Conn {
    let conn = this.cache.get(ws);
    if (!conn) {
      conn = ws.deserializeAttachment() as Conn;
      this.cache.set(ws, conn);
    }
    return conn;
  }

  private save(ws: WebSocket, conn: Conn): void {
    this.cache.set(ws, conn);
    ws.serializeAttachment(conn);
  }

  private send(ws: WebSocket, msg: object): void {
    try {
      ws.send(JSON.stringify(msg));
    } catch {
      return;
    }
  }

  private fail(ws: WebSocket, code: string, msg: string, close = false): void {
    this.send(ws, { t: "error", code, msg });
    if (close) void this.drop(ws, 1008, code);
  }

  private members(room: string, except?: WebSocket): WebSocket[] {
    return this.ctx.getWebSockets().filter((ws) => ws !== except && this.conn(ws).room === room);
  }

  private broadcast(room: string, msg: object, except?: WebSocket): void {
    for (const ws of this.members(room, except)) this.send(ws, msg);
  }

  private occupancy(room: string, except?: WebSocket): void {
    const sockets = this.members(room);
    for (const ws of sockets) if (ws !== except) this.send(ws, { t: "occupancy", others: sockets.length - 1 });
  }

  private enterRoom(ws: WebSocket, conn: Conn, room: string): void {
    conn.room = room;
    this.save(ws, conn);
    if (!room) {
      this.send(ws, { t: "peers", room, list: [] });
      this.send(ws, { t: "occupancy", others: 0 });
      return;
    }
    const peers = this.members(room, ws);
    const list = peers.map((peer) => {
      const other = this.conn(peer);
      return { pid: other.pid, name: other.name, star: other.star, pvp: other.pvp, models: other.models, pres: other.pres };
    });
    this.send(ws, { t: "peers", room, list });
    this.send(ws, { t: "occupancy", others: peers.length });
    this.broadcast(room, { t: "join", pid: conn.pid, name: conn.name, star: conn.star, pvp: conn.pvp, models: conn.models, pres: conn.pres }, ws);
    this.occupancy(room, ws);
  }

  private leaveRoom(ws: WebSocket, conn: Conn): void {
    const old = conn.room;
    conn.room = "";
    conn.pres = null;
    this.save(ws, conn);
    if (!old) return;
    this.broadcast(old, { t: "leave", pid: conn.pid });
    this.occupancy(old);
  }

  private async drop(ws: WebSocket, code = 1000, reason = ""): Promise<void> {
    const conn = this.conn(ws);
    if (conn.hello) {
      conn.hello = false;
      this.leaveRoom(ws, conn);
      this.leaveChallenges(ws, conn);
      this.save(ws, conn);
      if (conn.battle) await this.leaveBattle(ws, conn);
    }
    try {
      ws.close(code, reason);
    } catch {
      return;
    }
  }

  // Sets the alarm for `at` unless one is due sooner.
  private async scheduleAlarm(at: number): Promise<void> {
    const current = await this.ctx.storage.getAlarm();
    if (current === null || current > at) await this.ctx.storage.setAlarm(at);
  }

  // The alarm sweeps idle connections every idleMs / 2, ends challenges when they expire, forfeits battle members who stayed away and
  // forgets old forfeits.
  async alarm(): Promise<void> {
    const now = Date.now();
    for (const ws of this.ctx.getWebSockets()) {
      if (now - this.conn(ws).last > this.idleMs) await this.drop(ws, 1001, "idle");
    }
    this.expireChallenges(now);
    let due = Infinity;
    for (const [key, stored] of await this.ctx.storage.list<Away>({ prefix: "away:" })) {
      const pid = key.slice(5);
      const away = this.aways.get(pid) ?? stored;
      if (!away.forfeited && now >= away.deadline) await this.forfeit(pid, away);
      else if (away.forfeited && now >= away.keep) await this.setAway(pid, null);
      const record = this.aways.get(pid);
      if (record) due = Math.min(due, record.forfeited ? record.keep : record.deadline);
    }
    const sockets = this.ctx.getWebSockets();
    if (sockets.length === 0 && due === Infinity) return;
    let next = sockets.length ? now + this.idleMs / 2 : due;
    for (const ws of sockets) {
      const out = this.conn(ws).out;
      if (out) next = Math.min(next, out.expires);
    }
    await this.ctx.storage.setAlarm(Math.max(Math.min(next, due), now + 10));
  }

  async fetch(req: Request): Promise<Response> {
    const path = new URL(req.url).pathname;
    if (path.startsWith("/admin/")) return this.admin(req, path);
    if (req.headers.get("Upgrade") !== "websocket") return new Response("expected websocket", { status: 426 });

    const ip = req.headers.get("CF-Connecting-IP") ?? "local";
    if (ip !== "local" && this.ctx.getWebSockets().filter((ws) => this.conn(ws).ip === ip).length >= MAX_SOCKETS_PER_IP) {
      return new Response("too many connections", { status: 429 });
    }
    const pair = new WebSocketPair();
    const server = pair[1];
    this.ctx.acceptWebSocket(server);
    const conn: Conn = {
      banned: false, hello: false, admin: false, pid: "", name: "", star: 0, room: "", models: [], pres: null, pvp: false, out: null, battle: "",
      opponent: "", pvpSeq: 0, pvpSide: false, pvpN: 0, ip,
      last: Date.now(), presWin: 0, presN: 0, otherWin: 0, otherN: 0, battleWin: 0, battleN: 0, chats: [],
    };
    conn.banned = (await this.ctx.storage.get("ip:" + ip)) !== undefined;
    this.save(server, conn);
    await this.scheduleAlarm(Date.now() + this.idleMs / 2);
    return new Response(null, { status: 101, webSocket: pair[0] });
  }

  private async admin(req: Request, path: string): Promise<Response> {
    const token = this.env.ADMIN_TOKEN;
    if (!token) return json({ error: "admin disabled" }, 503);
    const ip = req.headers.get("CF-Connecting-IP") ?? "local";
    if (!this.authAllowed(ip)) return json({ error: "too many failed attempts" }, 429);
    const auth = req.headers.get("Authorization") ?? "";
    if (!auth.startsWith("Bearer ") || !(await sameSecret(auth.slice(7), token))) {
      this.authFailed(ip);
      return json({ error: "unauthorized" }, 401);
    }

    if (path === "/admin/players" && req.method === "GET") {
      const players = this.ctx.getWebSockets().map((ws) => this.conn(ws)).filter((conn) => conn.hello)
        .map((conn) => ({ pid: conn.pid, name: conn.name, room: conn.room, ip: conn.ip }));
      return json({ players });
    }
    if ((path === "/admin/ban" || path === "/admin/unban") && req.method === "POST") {
      let body: Msg;
      try {
        body = (await req.json()) as Msg;
      } catch {
        return json({ error: "bad json" }, 400);
      }
      const pid = typeof body.pid === "string" ? body.pid : "";
      const ip = typeof body.ip === "string" ? body.ip : "";
      if (!pid === !ip) return json({ error: "give exactly one of pid or ip" }, 400);
      const key = pid ? "pid:" + pid : "ip:" + ip;
      if (path === "/admin/ban") {
        await this.ctx.storage.put(key, true);
        for (const ws of this.ctx.getWebSockets()) {
          const conn = this.conn(ws);
          if ((pid && conn.pid === pid) || (ip && conn.ip === ip)) this.fail(ws, "banned", "banned", true);
        }
      } else {
        await this.ctx.storage.delete(key);
      }
      return json({ ok: true });
    }
    return json({ error: "not found" }, 404);
  }

  async webSocketMessage(ws: WebSocket, data: string | ArrayBuffer): Promise<void> {
    const conn = this.conn(ws);
    conn.last = Date.now();
    if (conn.banned) return this.fail(ws, "banned", "banned", true);
    if (typeof data !== "string" || data.length > MAX_SIDE_FRAME) return this.fail(ws, "bad", "bad frame");
    let msg: Msg;
    try {
      const parsed: unknown = JSON.parse(data);
      if (typeof parsed !== "object" || parsed === null || Array.isArray(parsed)) throw new Error("not an object");
      msg = parsed as Msg;
    } catch {
      return this.fail(ws, "bad", "bad json");
    }
    const type = msg.t;
    if (typeof type !== "string") return this.fail(ws, "bad", "missing t");
    if (data.length > MAX_FRAME && type !== "pvp_side") return this.fail(ws, "bad", "bad frame");

    if (!conn.hello) {
      if (type !== "hello") return this.drop(ws, 1008, "hello first");
      return this.onHello(ws, conn, msg);
    }
    if (!this.allow(ws, conn, type)) return;
    switch (type) {
      case "challenge":
        return this.onChallenge(ws, conn, msg);
      case "challenge_cancel":
        return this.onChallengeCancel(ws, conn);
      case "challenge_reply":
        return this.onChallengeReply(ws, conn, msg);
      case "chat":
        return this.onChat(ws, conn, msg);
      case "party":
        return this.onParty(ws, conn, msg);
      case "ping":
        return this.send(ws, { t: "pong" });
      case "pm":
        return this.onPm(ws, conn, msg);
      case "pres":
        return this.onPres(ws, conn, msg);
      case "profile":
        return this.onProfile(ws, conn, msg);
      case "pvp_cmd":
      case "pvp_hash":
        return this.onPvpStep(ws, conn, msg, type);
      case "pvp_end":
        return this.onPvpEnd(ws, conn, msg);
      case "pvp_resume":
        return this.onPvpResume(ws, conn, msg);
      case "pvp_side":
        return this.onPvpSide(ws, conn, msg);
      case "room":
        return this.onRoom(ws, conn, msg);
      default:
        return;
    }
  }

  async webSocketClose(ws: WebSocket, code: number): Promise<void> {
    await this.drop(ws, code === 1005 || code === 1006 ? 1000 : code);
  }

  async webSocketError(ws: WebSocket): Promise<void> {
    await this.drop(ws);
  }

  private allow(ws: WebSocket, conn: Conn, type: string): boolean {
    const now = conn.last;
    let ok = true;
    if (type === "chat" || type === "pm") {
      conn.chats = conn.chats.filter((ts) => now - ts < 10000);
    } else if (type === "pres") {
      if (now - conn.presWin >= 1000) {
        conn.presWin = now;
        conn.presN = 0;
      }
      ok = ++conn.presN <= PRES_PER_SEC;
    } else if (type === "pvp_cmd" || type === "pvp_hash" || type === "pvp_side" || type === "pvp_resume") {
      if (now - conn.battleWin >= 1000) {
        conn.battleWin = now;
        conn.battleN = 0;
      }
      ok = ++conn.battleN <= BATTLE_PER_SEC;
      if (!ok) this.send(ws, { t: "error", code: "rate", msg: "battle rate limit" });
    } else {
      if (now - conn.otherWin >= 1000) {
        conn.otherWin = now;
        conn.otherN = 0;
      }
      ok = ++conn.otherN <= OTHER_PER_SEC;
    }
    this.save(ws, conn);
    return ok;
  }

  private async onHello(ws: WebSocket, conn: Conn, msg: Msg): Promise<void> {
    if (msg.v !== 1) return this.fail(ws, "version", "protocol version 1 required", true);
    if (typeof msg.id !== "string" || !/^[0-9a-f]{32}$/.test(msg.id)) return this.fail(ws, "bad", "bad id", true);
    const pid = await hashId(msg.id);
    if (await this.ctx.storage.get("pid:" + pid)) return this.fail(ws, "banned", "banned", true);
    conn.pid = pid;
    conn.name = clean(msg.name, NAME_LEN) || "Player";
    conn.star = Math.min(999, Math.max(0, int(msg.star) ?? 0));
    conn.pvp = msg.pvp === true;
    const token = this.env.ADMIN_TOKEN;
    conn.admin = false;
    if (token && typeof msg.admin === "string" && this.authAllowed(conn.ip)) {
      conn.admin = await sameSecret(msg.admin, token);
      if (!conn.admin) this.authFailed(conn.ip);
    }
    conn.hello = true;
    this.save(ws, conn);
    this.send(ws, {
      t: "welcome", pid, admin: conn.admin,
      limits: { chat_len: CHAT_LEN, name_len: NAME_LEN, pres_hz: 10, chat_per_10s: CHAT_PER_10S },
    });
  }

  private onRoom(ws: WebSocket, conn: Conn, msg: Msg): void {
    if (typeof msg.room !== "string") return this.fail(ws, "bad", "bad room");
    const room = clean(msg.room, ROOM_LEN);
    if (conn.room) this.leaveRoom(ws, conn);
    this.enterRoom(ws, conn, room);
  }

  private onParty(ws: WebSocket, conn: Conn, msg: Msg): void {
    const models = parseModels(msg.models);
    if (!models) return this.fail(ws, "bad", "bad models");
    conn.models = models;
    this.save(ws, conn);
    if (conn.room) this.broadcast(conn.room, { t: "party", pid: conn.pid, models }, ws);
  }

  private onProfile(ws: WebSocket, conn: Conn, msg: Msg): void {
    const name = clean(msg.name, NAME_LEN);
    const star = int(msg.star);
    if (!name || star === null) return;
    conn.name = name;
    conn.star = Math.min(999, Math.max(0, star));
    if (typeof msg.pvp === "boolean") conn.pvp = msg.pvp;
    this.save(ws, conn);
    if (conn.room) this.broadcast(conn.room, { t: "profile", pid: conn.pid, name: conn.name, star: conn.star, pvp: conn.pvp }, ws);
  }

  private onPres(ws: WebSocket, conn: Conn, msg: Msg): void {
    const pres = parsePres(msg);
    if (!pres) return this.fail(ws, "bad", "bad pres");
    conn.pres = pres;
    this.save(ws, conn);
    if (conn.room) this.broadcast(conn.room, { t: "pres", pid: conn.pid, ...pres }, ws);
  }

  private onChat(ws: WebSocket, conn: Conn, msg: Msg): void {
    const scope = msg.scope;
    const text = clean(msg.text, CHAT_LEN);
    if (!text || (scope !== "room" && scope !== "global")) return;
    if (scope === "room" && !conn.room) return;
    if (conn.chats.length >= CHAT_PER_10S) return this.fail(ws, "rate", "chat rate limit");
    conn.chats.push(conn.last);
    this.save(ws, conn);

    const line: Line = { t: "chat", scope, pid: conn.pid, name: conn.name, text, ts: Date.now() };
    if (conn.admin) line.admin = true;
    if (scope === "global") {
      for (const peer of this.ctx.getWebSockets()) if (this.conn(peer).hello) this.send(peer, line);
      return;
    }
    this.broadcast(conn.room, line);
  }

  // The connected sockets of player `pid` (one player may run more than one client).
  private sockets(pid: string): WebSocket[] {
    return this.ctx.getWebSockets().filter((ws) => {
      const conn = this.conn(ws);
      return conn.hello && conn.pid === pid;
    });
  }

  private onPm(ws: WebSocket, conn: Conn, msg: Msg): void {
    if (!isPid(msg.to) || msg.to === conn.pid) return this.fail(ws, "bad", "bad pm");
    const text = clean(msg.text, CHAT_LEN);
    if (!text) return;
    if (conn.chats.length >= CHAT_PER_10S) return this.fail(ws, "rate", "chat rate limit");
    const targets = this.sockets(msg.to);
    if (targets.length === 0) return this.send(ws, { t: "error", code: "offline", msg: "player not online", pid: msg.to });
    conn.chats.push(conn.last);
    this.save(ws, conn);
    const to_name = this.conn(targets[0]).name;
    const line: Line = { t: "chat", scope: "pm", pid: conn.pid, name: conn.name, text, ts: Date.now(), to: msg.to, to_name };
    if (conn.admin) line.admin = true;
    for (const target of targets) this.send(target, line);
    for (const own of this.sockets(conn.pid)) this.send(own, line);
  }

  // Clears the challenge `ws` sent and tells the challenger (`toChallenger`) and the target (`toTarget`) that it ended with `reason`.
  private endChallenge(ws: WebSocket, conn: Conn, reason: string, toChallenger: boolean, toTarget: boolean): void {
    const out = conn.out;
    if (!out) return;
    conn.out = null;
    this.save(ws, conn);
    if (toChallenger) this.send(ws, { t: "challenge_end", pid: out.to, reason });
    if (toTarget) for (const target of this.sockets(out.to)) this.send(target, { t: "challenge_end", pid: conn.pid, reason });
  }

  // Ends every challenge whose time is up, with `timeout` to both sides.
  private expireChallenges(now: number): void {
    for (const ws of this.ctx.getWebSockets()) {
      const conn = this.conn(ws);
      if (conn.out && now >= conn.out.expires) this.endChallenge(ws, conn, "timeout", true, true);
    }
  }

  // The socket holding a pending challenge from `from` to `to`.
  private challengeBetween(from: string, to: string): WebSocket | null {
    for (const ws of this.ctx.getWebSockets()) {
      const conn = this.conn(ws);
      if (conn.hello && conn.pid === from && conn.out?.to === to) return ws;
    }
    return null;
  }

  // A player is busy while a challenge they sent or one sent to them is pending, or while they are in a battle.
  private busy(pid: string): boolean {
    for (const ws of this.ctx.getWebSockets()) {
      const conn = this.conn(ws);
      if (!conn.hello) continue;
      if (conn.pid === pid && (conn.out || conn.battle)) return true;
      if (conn.out?.to === pid) return true;
    }
    return false;
  }

  private async onChallenge(ws: WebSocket, conn: Conn, msg: Msg): Promise<void> {
    const now = Date.now();
    this.expireChallenges(now);
    const card = parseCard(msg.card);
    const data = parseData(msg.data);
    if (!isPid(msg.to) || msg.to === conn.pid || !card || data === null) return this.fail(ws, "bad", "bad challenge");
    const to = msg.to;
    const refuse = (reason: string) => this.send(ws, { t: "challenge_end", pid: to, reason });
    if (this.busy(conn.pid)) return refuse(conn.battle ? "busy" : "pending");
    const targets = this.sockets(to);
    if (targets.length === 0) return refuse("left");
    if (!conn.pvp || !targets.some((target) => this.conn(target).pvp)) return refuse("off");
    if (this.busy(to)) return refuse("busy");
    for (const [key, at] of this.cooldowns) if (now - at >= COOLDOWN_MS) this.cooldowns.delete(key);
    const pair = conn.pid + ">" + to;
    if (this.cooldowns.has(pair)) return refuse("cooldown");

    const out: Outgoing = { to, expires: now + this.challengeMs, card, ver: clean(msg.ver, VER_LEN), data };
    if (encoder.encode(JSON.stringify({ ...conn, out })).length > MAX_ATTACHMENT) return this.fail(ws, "bad", "challenge too large");
    if (this.cooldowns.size > 10000) this.cooldowns.clear();
    this.cooldowns.set(pair, now);
    conn.out = out;
    this.save(ws, conn);
    for (const target of targets) this.send(target, { t: "challenge", from: conn.pid, name: conn.name, star: conn.star, card, expires: this.challengeMs });
    await this.scheduleAlarm(out.expires);
  }

  private onChallengeCancel(ws: WebSocket, conn: Conn): void {
    this.expireChallenges(Date.now());
    this.endChallenge(ws, conn, "cancelled", false, true);
  }

  private onChallengeReply(ws: WebSocket, conn: Conn, msg: Msg): void {
    this.expireChallenges(Date.now());
    if (!isPid(msg.from) || typeof msg.accept !== "boolean") return this.fail(ws, "bad", "bad reply");
    const challengerWs = this.challengeBetween(msg.from, conn.pid);
    if (!challengerWs) return;
    const challenger = this.conn(challengerWs);
    const out = challenger.out as Outgoing;
    if (!msg.accept) {
      const reason = REFUSALS.find((known) => known === msg.reason) ?? "declined";
      return this.endChallenge(challengerWs, challenger, reason, true, false);
    }
    const card = parseCard(msg.card);
    const data = parseData(msg.data);
    if (!card || data === null) return this.fail(ws, "bad", "bad reply");
    if (clean(msg.ver, VER_LEN) !== out.ver) return this.endChallenge(challengerWs, challenger, "version", true, true);
    if (data && out.data && data !== out.data) return this.endChallenge(challengerWs, challenger, "data", true, true);

    const battle = randomHex(8);
    const seed = crypto.getRandomValues(new Uint32Array(1))[0];
    const first = crypto.getRandomValues(new Uint8Array(1))[0] & 1 ? challenger.pid : conn.pid;
    challenger.out = null;
    challenger.battle = conn.battle = battle;
    challenger.opponent = conn.pid;
    conn.opponent = challenger.pid;
    this.save(challengerWs, challenger);
    this.save(ws, conn);
    const start = { t: "pvp_start", battle, seed, first, opening_stamina: this.openingStamina };
    this.send(challengerWs, { ...start, opponent: conn.pid, name: conn.name, star: conn.star, card });
    this.send(ws, { ...start, opponent: challenger.pid, name: challenger.name, star: challenger.star, card: out.card });
  }

  // A member's end of its battle frees that member. A result without a winner tells an opponent still in the battle (pvp_gone `aborted`);
  // an opponent who is away has nothing left to come back to.
  private async onPvpEnd(ws: WebSocket, conn: Conn, msg: Msg): Promise<void> {
    if (typeof msg.battle !== "string" || !RESULTS.some((known) => known === msg.result)) return this.fail(ws, "bad", "bad pvp_end");
    if (!conn.battle || msg.battle !== conn.battle) return;
    const { battle, opponent } = conn;
    this.leaveLink(ws, conn);
    if (msg.result === "aborted" || msg.result === "desync") {
      const target = this.battleSocket(opponent, battle);
      if (target) this.send(target, { t: "pvp_gone", battle, reason: "aborted" });
    }
    const away = await this.away(opponent);
    if (away && away.battle === battle && !away.forfeited) await this.setAway(opponent, null);
  }

  // Clears a member's battle and its link state.
  private leaveLink(ws: WebSocket, conn: Conn): void {
    this.outboxes.delete(conn.pid);
    conn.battle = conn.opponent = "";
    conn.pvpSeq = conn.pvpN = 0;
    conn.pvpSide = false;
    this.save(ws, conn);
  }

  // The socket of `pid` that holds `battle`.
  private battleSocket(pid: string, battle: string): WebSocket | null {
    return this.sockets(pid).find((ws) => this.conn(ws).battle === battle) ?? null;
  }

  private async away(pid: string): Promise<Away | null> {
    if (!this.aways.has(pid)) this.aways.set(pid, (await this.ctx.storage.get<Away>("away:" + pid)) ?? null);
    return this.aways.get(pid) ?? null;
  }

  private async setAway(pid: string, away: Away | null): Promise<void> {
    if (this.aways.size > 10000) this.aways.clear();
    this.aways.set(pid, away);
    if (away) await this.ctx.storage.put("away:" + pid, away);
    else await this.ctx.storage.delete("away:" + pid);
  }

  // A battle message names the sender's own battle.
  private member(ws: WebSocket, conn: Conn, msg: Msg): boolean {
    if (!isBattle(msg.battle)) {
      this.fail(ws, "bad", "bad battle");
      return false;
    }
    if (msg.battle === conn.battle) return true;
    this.fail(ws, "bad", "not in this battle");
    return false;
  }

  // Drops the messages `pid` acknowledged (relay numbers up to `ack`).
  private acknowledge(pid: string, ack: unknown): void {
    const upto = int(ack);
    const outbox = this.outboxes.get(pid);
    if (upto === null || !outbox) return;
    while (outbox.length && (outbox[0].n as number) <= upto) outbox.shift();
  }

  // Passes a battle message to the sender's opponent with the next relay number: to its battle socket and its outbox, or, while it is away,
  // to the outbox of its away record (an outbox that grows past its limits forfeits the member who stays away).
  private async relay(conn: Conn, msg: Msg): Promise<void> {
    const target = this.battleSocket(conn.opponent, conn.battle);
    if (target) {
      const other = this.conn(target);
      const out = { ...msg, n: ++other.pvpN };
      this.save(target, other);
      const outbox = this.outboxes.get(other.pid) ?? [];
      outbox.push(out);
      while (outbox.length > OUTBOX_MAX) outbox.shift();
      this.outboxes.set(other.pid, outbox);
      this.send(target, out);
      return;
    }
    const away = await this.away(conn.opponent);
    if (!away || away.battle !== conn.battle || away.forfeited) return;
    away.outbox.push({ ...msg, n: ++away.n });
    if (away.outbox.length > OUTBOX_MAX || JSON.stringify(away.outbox).length > OUTBOX_CHARS) return this.forfeit(conn.opponent, away);
    await this.setAway(conn.opponent, away);
  }

  // pvp_side: once per member, relayed as it is.
  private async onPvpSide(ws: WebSocket, conn: Conn, msg: Msg): Promise<void> {
    if (!this.member(ws, conn, msg)) return;
    if (!isSide(msg.side)) return this.fail(ws, "bad", "bad side");
    if (conn.pvpSide) return this.fail(ws, "bad", "side already sent");
    conn.pvpSide = true;
    this.save(ws, conn);
    await this.relay(conn, { t: "pvp_side", battle: conn.battle, side: msg.side });
  }

  // pvp_cmd and pvp_hash: one per command the sender's game applied, seq counting up by one.
  private async onPvpStep(ws: WebSocket, conn: Conn, msg: Msg, type: string): Promise<void> {
    if (!this.member(ws, conn, msg)) return;
    const seq = parseSeq(msg.seq);
    if (seq === null || !isHash(msg.hash) || (type === "pvp_cmd" && !isCmd(msg.cmd))) return this.fail(ws, "bad", "bad " + type);
    this.acknowledge(conn.pid, msg.ack);
    if (seq !== conn.pvpSeq + 1) return this.fail(ws, "bad", "seq out of order");
    conn.pvpSeq = seq;
    this.save(ws, conn);
    const out: Msg = { t: type, battle: conn.battle, seq, hash: msg.hash };
    if (type === "pvp_cmd") out.cmd = msg.cmd;
    await this.relay(conn, out);
  }

  // pvp_resumed with the link counters the server holds for the member, then every message it has not acknowledged.
  private deliver(ws: WebSocket, conn: Conn, ack: unknown): void {
    this.send(ws, { t: "pvp_resumed", battle: conn.battle, seq: conn.pvpSeq, side: conn.pvpSide });
    this.acknowledge(conn.pid, ack);
    for (const out of this.outboxes.get(conn.pid) ?? []) this.send(ws, out);
  }

  // A member's game that still holds the battle comes back: from another socket that still holds it (that one is closed), or within
  // the away time. Otherwise pvp_gone: `forfeit` when the time ran out, `closed` when the battle is gone.
  private async onPvpResume(ws: WebSocket, conn: Conn, msg: Msg): Promise<void> {
    if (!isBattle(msg.battle)) return this.fail(ws, "bad", "bad battle");
    const battle = msg.battle;
    if (conn.battle) return conn.battle === battle ? this.deliver(ws, conn, msg.ack) : this.fail(ws, "bad", "not in this battle");
    const gone = (reason: string) => this.send(ws, { t: "pvp_gone", battle, reason });
    const holder = this.battleSocket(conn.pid, battle);
    if (holder) {
      const old = this.conn(holder);
      Object.assign(conn, { battle, opponent: old.opponent, pvpSeq: old.pvpSeq, pvpSide: old.pvpSide, pvpN: old.pvpN });
      old.battle = old.opponent = "";
      this.save(holder, old);
      this.save(ws, conn);
      try {
        holder.close(1000, "replaced");
      } catch {
        // A socket that is already closing refuses the close.
      }
      return this.deliver(ws, conn, msg.ack);
    }
    const away = await this.away(conn.pid);
    if (!away || away.battle !== battle) return gone("closed");
    if (away.forfeited) return gone("forfeit");
    if (Date.now() >= away.deadline) {
      await this.forfeit(conn.pid, away);
      return gone("forfeit");
    }
    const target = this.battleSocket(away.opponent, battle);
    await this.setAway(conn.pid, null);
    if (!target) return gone("closed");
    Object.assign(conn, { battle, opponent: away.opponent, pvpSeq: away.seq, pvpSide: away.side, pvpN: away.n });
    this.save(ws, conn);
    this.outboxes.set(conn.pid, away.outbox);
    this.deliver(ws, conn, msg.ack);
    this.send(target, { t: "pvp_back", battle });
  }

  // A battle member's connection closed: its opponent, if still in the battle, is told it is away and waits awayMs for it; when the
  // opponent is away too, the battle is dropped.
  private async leaveBattle(ws: WebSocket, conn: Conn): Promise<void> {
    const { battle, opponent } = conn;
    const away: Away = {
      battle, opponent, deadline: Date.now() + this.awayMs, seq: conn.pvpSeq, side: conn.pvpSide, n: conn.pvpN,
      outbox: this.outboxes.get(conn.pid) ?? [], forfeited: false, keep: 0,
    };
    this.leaveLink(ws, conn);
    const target = this.battleSocket(opponent, battle);
    if (target) {
      await this.setAway(conn.pid, away);
      this.send(target, { t: "pvp_away", battle, ms: this.awayMs });
      await this.scheduleAlarm(away.deadline);
      return;
    }
    const other = await this.away(opponent);
    if (other && other.battle === battle && !other.forfeited) await this.setAway(opponent, null);
  }

  // The away member lost: its opponent wins (pvp_gone `left`) and leaves the battle; the record is kept a while for a late resume.
  private async forfeit(pid: string, away: Away): Promise<void> {
    const target = this.battleSocket(away.opponent, away.battle);
    if (target) {
      this.send(target, { t: "pvp_gone", battle: away.battle, reason: "left" });
      this.leaveLink(target, this.conn(target));
    }
    away.forfeited = true;
    away.outbox = [];
    away.keep = Date.now() + GONE_KEEP_MS;
    await this.setAway(pid, away);
    await this.scheduleAlarm(away.keep);
  }

  // A player who disconnects ends the challenge they sent and, when no other client of theirs is left, the ones sent to them, both with
  // `left`. drop() then leaves the player's battle (leaveBattle).
  private leaveChallenges(ws: WebSocket, conn: Conn): void {
    this.endChallenge(ws, conn, "left", false, true);
    if (!this.sockets(conn.pid).some((other) => other !== ws)) {
      for (const peer of this.ctx.getWebSockets()) {
        const other = this.conn(peer);
        if (other.hello && other.out?.to === conn.pid) this.endChallenge(peer, other, "left", true, false);
      }
    }
  }
}
