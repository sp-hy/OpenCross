// Protocol conformance test for the multiplayer server.
// Usage: node protocol.mjs <ws-url> <admin-token> [idle-ms]
// Drives a running server through docs/formats/multiplayer.md and exits non-zero on any failure.
import { createHash, randomBytes } from "node:crypto";

const [url, token, idleArg] = process.argv.slice(2);
if (!url || !token) {
  console.error("usage: node protocol.mjs <ws-url> <admin-token> [idle-ms]");
  process.exit(2);
}
const httpBase = url.replace(/^ws/, "http").replace(/\/ws$/, "");
const idleMs = Number(idleArg) || 0;
const run = randomBytes(3).toString("hex");

let passed = 0;
let failed = 0;
function check(name, ok, detail = "") {
  if (ok) passed++;
  else {
    failed++;
    console.log(`FAIL ${name} ${detail}`);
  }
}

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const randomId = () => randomBytes(16).toString("hex");
const pidOf = (id) => createHash("sha256").update(id).digest("hex").slice(0, 16);

class Client {
  constructor(ws) {
    this.ws = ws;
    this.inbox = [];
    this.closed = false;
    this.closeCode = 0;
    ws.addEventListener("message", (event) => this.inbox.push(JSON.parse(event.data)));
    ws.addEventListener("close", (event) => {
      this.closed = true;
      this.closeCode = event.code;
    });
  }

  static async open() {
    const ws = new WebSocket(url);
    const client = new Client(ws);
    await new Promise((resolve, reject) => {
      ws.addEventListener("open", resolve);
      ws.addEventListener("error", () => reject(new Error("connect failed")));
    });
    return client;
  }

  static async join(name, room, extra = {}) {
    const client = await Client.open();
    client.id = extra.id ?? randomId();
    const hello = { t: "hello", v: 1, id: client.id, name, star: extra.star ?? 3, ver: "test" };
    if (extra.admin !== undefined) hello.admin = extra.admin;
    client.send(hello);
    client.welcome = await client.take("welcome");
    await client.take("history");
    if (room !== undefined) {
      client.send({ t: "room", room });
      await client.take("peers");
      await client.take("occupancy");
      await client.take("history");
    }
    return client;
  }

  send(msg) {
    this.ws.send(JSON.stringify(msg));
  }

  async take(type, ms = 2000, pred = () => true) {
    const end = Date.now() + ms;
    while (Date.now() < end) {
      const index = this.inbox.findIndex((m) => m.t === type && pred(m));
      if (index >= 0) return this.inbox.splice(index, 1)[0];
      await sleep(10);
    }
    return null;
  }

  async quiet(ms, type) {
    await sleep(ms);
    return this.inbox.filter((m) => !type || m.t === type).length === 0;
  }

  async waitClosed(ms = 2000) {
    const end = Date.now() + ms;
    while (!this.closed && Date.now() < end) await sleep(10);
    return this.closed;
  }

  close() {
    this.ws.close();
  }
}

async function admin(path, options = {}, auth = `Bearer ${token}`) {
  const headers = { ...(auth ? { Authorization: auth } : {}), "content-type": "application/json" };
  return fetch(httpBase + path, { ...options, headers });
}

const pres = (seq, extra = {}) => ({ t: "pres", x: 10, y: 20, z: 30, f: 40, c: 1, m: 1, s: 0, seq, ...extra });

async function testHandshake() {
  const first = await Client.open();
  first.send({ t: "chat", scope: "global", text: "too early" });
  check("hello first closes", await first.waitClosed());

  const id = randomId();
  const a = await Client.open();
  a.send({ t: "hello", v: 1, id, name: "  Al\u0001ice  ", star: 5, ver: "x" });
  const welcome = await a.take("welcome");
  check("welcome pid", welcome?.pid === pidOf(id), JSON.stringify(welcome));
  check(
    "welcome limits",
    welcome?.limits?.chat_len === 200 && welcome.limits.name_len === 24 && welcome.limits.pres_hz === 10 && welcome.limits.chat_per_10s === 5,
  );
  const history = await a.take("history");
  check("global history after welcome", history?.scope === "global" && Array.isArray(history.lines));
  a.send({ t: "ping" });
  check("pong", (await a.take("pong")) !== null);
  a.send({ t: "mystery", q: 1 });
  a.send({ t: "ping" });
  check("unknown t ignored", (await a.take("pong")) !== null && a.inbox.length === 0 && !a.closed);
  a.close();

  const old = await Client.open();
  old.send({ t: "hello", v: 2, id: randomId(), name: "x", star: 0, ver: "x" });
  const error = await old.take("error");
  check("error version", error?.code === "version" && (await old.waitClosed()));

  const bad = await Client.open();
  bad.send({ t: "hello", v: 1, id: randomId(), name: "x", star: 0 });
  await bad.take("welcome");
  bad.ws.send("not json");
  check("error bad", (await bad.take("error"))?.code === "bad");
  bad.close();
}

async function testRooms() {
  const room = `room_${run}`;
  const other = `other_${run}`;
  const a = await Client.join("Alice", room);
  a.send({ t: "party", models: [1, 2, 3] });
  a.send(pres(1));
  await sleep(100);

  const b = await Client.open();
  b.id = randomId();
  b.send({ t: "hello", v: 1, id: b.id, name: "Bob", star: 7, ver: "x" });
  b.welcome = await b.take("welcome");
  await b.take("history");
  b.send({ t: "room", room });
  const peers = await b.take("peers");
  check("peers lists a", peers?.room === room && peers.list.length === 1 && peers.list[0].name === "Alice");
  check("peers entry fields", peers?.list[0]?.models.join() === "1,2,3" && peers.list[0].star === 3 && peers.list[0].pres?.x === 10);
  check("peers pid", peers?.list[0]?.pid === a.welcome.pid);
  check("occupancy with peers", (await b.take("occupancy"))?.others === 1);
  check("room history after peers", (await b.take("history"))?.scope === "room");

  const join = await a.take("join");
  check("join relayed", join?.pid === b.welcome.pid && join.name === "Bob" && join.star === 7 && join.pres === null);
  check("occupancy on join", (await a.take("occupancy"))?.others === 1);

  const c = await Client.join("Carol", other);
  b.send({ t: "party", models: [9] });
  b.send(pres(1, { x: 99 }));
  b.send({ t: "profile", name: "Bobby", star: 8 });
  check("party relayed", (await a.take("party"))?.models.join() === "9");
  check("pres relayed", (await a.take("pres"))?.x === 99);
  const profile = await a.take("profile");
  check("profile relayed", profile?.name === "Bobby" && profile.star === 8 && profile.pid === b.welcome.pid);
  check("others get nothing", await c.quiet(300));
  check("sender gets no echo", b.inbox.every((m) => !["pres", "party", "profile"].includes(m.t)));

  const d = await Client.open();
  d.id = randomId();
  d.send({ t: "hello", v: 1, id: d.id, name: "Dan", star: 1, ver: "x" });
  await d.take("welcome");
  d.send({ t: "room", room });
  const peers2 = await d.take("peers");
  const bob = peers2?.list.find((p) => p.name === "Bobby");
  check("last pres handed to joiner", bob?.pres?.x === 99 && bob.pres.seq === 1 && peers2.list.length === 2);

  b.send({ t: "room", room: other });
  check("leave on room change", (await a.take("leave", 2000, (m) => m.pid === b.welcome.pid)) !== null);
  check("occupancy on leave", (await a.take("occupancy", 2000, (m) => m.others === 1)) !== null);
  check("old room gets no pres", (b.send(pres(2)), await a.quiet(300, "pres")));
  check("new room sees join", (await c.take("join"))?.pid === b.welcome.pid);

  c.close();
  check("leave on disconnect", (await b.take("leave"))?.pid === c.welcome.pid);
  b.send({ t: "room", room: "" });
  await b.take("peers");
  for (const client of [a, b, d]) client.close();
}

async function testChat() {
  const room = `chat_${run}`;
  const a = await Client.join("A", room);
  const b = await Client.join("B", room);
  const c = await Client.join("C", `chat2_${run}`);
  await a.take("join");
  a.inbox.length = 0;
  b.inbox.length = 0;

  a.send({ t: "chat", scope: "room", text: "  hi\u0007 there\n" });
  const line = await b.take("chat");
  check("room chat reaches room", line?.scope === "room" && line.text === "hi there" && line.name === "A" && line.pid === a.welcome.pid && line.ts > 1e12);
  check("room chat echoes sender", (await a.take("chat"))?.text === "hi there");
  check("room chat not outside room", await c.quiet(300, "chat"));

  a.send({ t: "chat", scope: "global", text: "everyone" });
  check("global reaches all", (await c.take("chat"))?.text === "everyone" && (await b.take("chat"))?.text === "everyone");

  a.inbox.length = 0;
  a.send({ t: "chat", scope: "global", text: "\u0001\u0002  " });
  a.send({ t: "chat", scope: "global", text: "" });
  check("empty chat dropped", await a.quiet(300, "chat"));

  a.send({ t: "chat", scope: "global", text: "x".repeat(300) });
  check("chat trimmed to 200", (await a.take("chat"))?.text.length === 200);
  a.send({ t: "chat", scope: "global", text: "\u{1F600}".repeat(300) });
  check("chat counts code points", Array.from((await a.take("chat"))?.text ?? "").length === 200);
  a.send({ t: "profile", name: "n".repeat(40), star: 1 });
  a.send({ t: "room", room: `chat_${run}` });
  const join = await b.take("join", 2000, (m) => m.name.length === 24);
  check("name trimmed to 24", join !== null);

  const fast = await Client.join("Fast", room);
  for (let i = 0; i < 6; i++) fast.send({ t: "chat", scope: "room", text: `m${i}` });
  const error = await fast.take("error");
  check("chat rate limit", error?.code === "rate");
  await sleep(200);
  check("exactly five delivered", fast.inbox.filter((m) => m.t === "chat").length === 5);
  for (const client of [a, b, c, fast]) client.close();
}

async function sendMany(scope, room, prefix, count) {
  let client = null;
  for (let i = 0; i < count; i++) {
    if (i % 5 === 0) {
      client?.close();
      client = await Client.join("Hist", room);
    }
    client.send({ t: "chat", scope, text: `${prefix}${String(i).padStart(2, "0")}` });
    await client.take("chat");
  }
  client.close();
}

async function testHistory() {
  const room = `hist_${run}`;
  await sendMany("room", room, `r${run}-`, 23);
  await sendMany("global", undefined, `g${run}-`, 23);
  const probe = await Client.open();
  probe.send({ t: "hello", v: 1, id: randomId(), name: "P", star: 0, ver: "x" });
  await probe.take("welcome");
  const global = await probe.take("history");
  const texts = global.lines.map((l) => l.text);
  const expected = Array.from({ length: 20 }, (_, i) => `g${run}-${String(i + 3).padStart(2, "0")}`);
  check("global history last 20 oldest first", texts.length === 20 && texts.join() === expected.join(), texts.join());
  check("history lines shaped like chat", global.lines.every((l) => l.t === "chat" && l.scope === "global" && typeof l.ts === "number"));
  probe.send({ t: "room", room });
  await probe.take("peers");
  const roomHistory = await probe.take("history");
  const roomTexts = roomHistory.lines.map((l) => l.text);
  const roomExpected = Array.from({ length: 20 }, (_, i) => `r${run}-${String(i + 3).padStart(2, "0")}`);
  check("room history last 20 oldest first", roomHistory.scope === "room" && roomTexts.join() === roomExpected.join(), roomTexts.join());
  probe.close();
}

async function testPresRate() {
  const room = `rate_${run}`;
  const a = await Client.join("A", room);
  const b = await Client.join("B", room);
  await a.take("join");
  for (let i = 0; i < 60; i++) a.send(pres(i));
  await sleep(500);
  const got = b.inbox.filter((m) => m.t === "pres").length;
  check("pres over 15/s dropped", got >= 10 && got <= 30, `got ${got}`);
  check("pres drops are silent", a.inbox.every((m) => m.t !== "error"));
  a.close();
  b.close();
}

async function testAdmin() {
  const noAuth = await admin("/admin/players", {}, null);
  check("admin needs auth", noAuth.status === 401);
  const wrong = await admin("/admin/players", {}, "Bearer nope");
  check("admin wrong token", wrong.status === 401);

  const id = randomId();
  const victim = await Client.join("Victim", `ban_${run}`, { id });
  const players = await (await admin("/admin/players")).json();
  const entry = players.players.find((p) => p.pid === pidOf(id));
  check("players lists pid, name, room, ip", entry?.name === "Victim" && entry.room === `ban_${run}` && typeof entry.ip === "string", JSON.stringify(entry));

  const ban = await admin("/admin/ban", { method: "POST", body: JSON.stringify({ pid: pidOf(id) }) });
  check("ban pid ok", ban.status === 200);
  check("banned player told and closed", (await victim.take("error"))?.code === "banned" && (await victim.waitClosed()));
  const again = await Client.open();
  again.send({ t: "hello", v: 1, id, name: "Victim", star: 0, ver: "x" });
  check("banned pid refused", (await again.take("error"))?.code === "banned" && (await again.waitClosed()));
  await admin("/admin/unban", { method: "POST", body: JSON.stringify({ pid: pidOf(id) }) });
  const back = await Client.join("Victim", undefined, { id });
  check("unban pid", back.welcome?.pid === pidOf(id));
  back.close();

  const ip = entry.ip;
  await admin("/admin/ban", { method: "POST", body: JSON.stringify({ ip }) });
  const blocked = await Client.open();
  blocked.send({ t: "hello", v: 1, id: randomId(), name: "Blocked", star: 0, ver: "x" });
  check("banned ip refused", (await blocked.take("error"))?.code === "banned" && (await blocked.waitClosed()));
  await admin("/admin/unban", { method: "POST", body: JSON.stringify({ ip }) });
  const free = await Client.join("Free", undefined);
  check("unban ip", free.welcome !== null);
  free.close();

  const empty = await admin("/admin/ban", { method: "POST", body: "{}" });
  check("ban needs pid or ip", empty.status === 400);
}

async function testAdminChat() {
  const room = `admin_${run}`;
  const admin = await Client.join("Op", room, { admin: token });
  const guest = await Client.join("Guest", room, { admin: "wrong" });
  check("admin token accepted", admin.welcome?.admin === true);
  check("wrong admin token refused", guest.welcome?.admin === false);
  await admin.take("join");
  admin.inbox.length = 0;
  guest.inbox.length = 0;
  admin.send({ t: "chat", scope: "room", text: "from the admin" });
  check("admin chat marked", (await guest.take("chat"))?.admin === true);
  guest.send({ t: "chat", scope: "room", text: "from a guest" });
  const plain = await admin.take("chat", 2000, (m) => m.text === "from a guest");
  check("guest chat not marked", plain !== undefined && plain.admin === undefined);
  admin.close();
  guest.close();
}

async function testPresClamp() {
  const room = `clamp_${run}`;
  const a = await Client.join("A", room);
  const b = await Client.join("B", room);
  await a.take("join");
  a.send(pres(1, { x: 1e15, z: -1e15 }));
  const got = await b.take("pres");
  check("pres clamped", got?.x === 2147483647 && got.z === -2147483647);
  a.close();
  b.close();
}

// Last: it locks the test's own address out of admin for the rest of the window.
async function testAuthLockout() {
  for (let i = 0; i < 10; i++) await admin("/admin/players", {}, "Bearer wrong");
  const locked = await admin("/admin/players");
  check("admin locked after repeated wrong tokens", locked.status === 429);
  const late = await Client.join("Late", undefined, { admin: token });
  check("hello admin refused while locked", late.welcome?.admin === false);
  late.close();
}

async function testIdle() {
  if (!idleMs) return;
  const idle = await Client.join("Idle", undefined);
  const busy = await Client.join("Busy", undefined);
  const end = Date.now() + idleMs * 2.5;
  while (Date.now() < end) {
    busy.send({ t: "ping" });
    await sleep(idleMs / 4);
  }
  check("idle connection closed", idle.closed);
  check("active connection kept", !busy.closed);
  busy.close();
}

const tests = [testHandshake, testRooms, testChat, testHistory, testPresRate, testAdmin, testAdminChat, testPresClamp, testIdle, testAuthLockout];
for (const test of tests) {
  try {
    await test();
  } catch (error) {
    failed++;
    console.log(`FAIL ${test.name} threw ${error?.stack ?? error}`);
  }
}
console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
