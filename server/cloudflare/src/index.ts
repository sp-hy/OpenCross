// OpenCross multiplayer server: one Durable Object holds every connection, room,
// last presence and chat history, and speaks the protocol in docs/formats/multiplayer.md.
import { DurableObject } from "cloudflare:workers";

interface Env {
  WORLD: DurableObjectNamespace;
  ADMIN_TOKEN?: string;
  IDLE_MS?: string;
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
  ip: string;
  last: number;
  presWin: number;
  presN: number;
  otherWin: number;
  otherN: number;
  chats: number[];
}

interface Line {
  t: "chat";
  scope: "room" | "global";
  pid: string;
  name: string;
  text: string;
  ts: number;
  admin?: true;
}

type Msg = Record<string, unknown>;

const NAME_LEN = 24;
const CHAT_LEN = 200;
const ROOM_LEN = 64;
const HISTORY = 20;
const MAX_ROOM_HISTORIES = 256;
const MAX_FRAME = 4096;
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
  private globalHistory: Line[] = [];
  private roomHistory = new Map<string, Line[]>();
  private authFailures = new Map<string, number[]>();

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
    if (close) this.drop(ws, 1008, code);
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
      return { pid: other.pid, name: other.name, star: other.star, models: other.models, pres: other.pres };
    });
    this.send(ws, { t: "peers", room, list });
    this.send(ws, { t: "occupancy", others: peers.length });
    this.send(ws, { t: "history", scope: "room", lines: this.roomHistory.get(room) ?? [] });
    this.broadcast(room, { t: "join", pid: conn.pid, name: conn.name, star: conn.star, models: conn.models, pres: conn.pres }, ws);
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

  private drop(ws: WebSocket, code = 1000, reason = ""): void {
    const conn = this.conn(ws);
    if (conn.hello) {
      this.leaveRoom(ws, conn);
      conn.hello = false;
      this.save(ws, conn);
    }
    try {
      ws.close(code, reason);
    } catch {
      return;
    }
  }

  private async scheduleSweep(): Promise<void> {
    if ((await this.ctx.storage.getAlarm()) === null) {
      await this.ctx.storage.setAlarm(Date.now() + this.idleMs / 2);
    }
  }

  async alarm(): Promise<void> {
    const now = Date.now();
    for (const ws of this.ctx.getWebSockets()) {
      if (now - this.conn(ws).last > this.idleMs) this.drop(ws, 1001, "idle");
    }
    if (this.ctx.getWebSockets().length > 0) await this.ctx.storage.setAlarm(now + this.idleMs / 2);
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
      banned: false, hello: false, admin: false, pid: "", name: "", star: 0, room: "", models: [], pres: null, ip,
      last: Date.now(), presWin: 0, presN: 0, otherWin: 0, otherN: 0, chats: [],
    };
    conn.banned = (await this.ctx.storage.get("ip:" + ip)) !== undefined;
    this.save(server, conn);
    await this.scheduleSweep();
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
    if (typeof data !== "string" || data.length > MAX_FRAME) return this.fail(ws, "bad", "bad frame");
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

    if (!conn.hello) {
      if (type !== "hello") return this.drop(ws, 1008, "hello first");
      return this.onHello(ws, conn, msg);
    }
    if (!this.allow(ws, conn, type)) return;
    switch (type) {
      case "chat":
        return this.onChat(ws, conn, msg);
      case "party":
        return this.onParty(ws, conn, msg);
      case "ping":
        return this.send(ws, { t: "pong" });
      case "pres":
        return this.onPres(ws, conn, msg);
      case "profile":
        return this.onProfile(ws, conn, msg);
      case "room":
        return this.onRoom(ws, conn, msg);
      default:
        return;
    }
  }

  async webSocketClose(ws: WebSocket, code: number): Promise<void> {
    this.drop(ws, code === 1005 || code === 1006 ? 1000 : code);
  }

  async webSocketError(ws: WebSocket): Promise<void> {
    this.drop(ws);
  }

  private allow(ws: WebSocket, conn: Conn, type: string): boolean {
    const now = conn.last;
    let ok = true;
    if (type === "chat") {
      conn.chats = conn.chats.filter((ts) => now - ts < 10000);
    } else if (type === "pres") {
      if (now - conn.presWin >= 1000) {
        conn.presWin = now;
        conn.presN = 0;
      }
      ok = ++conn.presN <= PRES_PER_SEC;
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
    this.send(ws, { t: "history", scope: "global", lines: this.globalHistory });
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
    this.save(ws, conn);
    if (conn.room) this.broadcast(conn.room, { t: "profile", pid: conn.pid, name: conn.name, star: conn.star }, ws);
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
      this.remember(this.globalHistory, line);
      for (const peer of this.ctx.getWebSockets()) if (this.conn(peer).hello) this.send(peer, line);
      return;
    }
    let lines = this.roomHistory.get(conn.room);
    if (!lines) {
      if (this.roomHistory.size >= MAX_ROOM_HISTORIES) {
        const oldest = this.roomHistory.keys().next().value;
        if (oldest !== undefined) this.roomHistory.delete(oldest);
      }
      lines = [];
      this.roomHistory.set(conn.room, lines);
    }
    this.remember(lines, line);
    this.broadcast(conn.room, line);
  }

  private remember(lines: Line[], line: Line): void {
    lines.push(line);
    if (lines.length > HISTORY) lines.shift();
  }
}
