// Protocol conformance test for the multiplayer server.
// Usage: node protocol.mjs <ws-url> <admin-token> [idle-ms] [challenge-ms] [opening-stamina] [away-ms]
// Drives a running server through docs/formats/multiplayer.md and exits non-zero on any failure.
import { createHash, randomBytes } from "node:crypto";

const [url, token, idleArg, challengeArg, staminaArg, awayArg] = process.argv.slice(2);
if (!url || !token) {
  console.error("usage: node protocol.mjs <ws-url> <admin-token> [idle-ms]");
  process.exit(2);
}
const httpBase = url.replace(/^ws/, "http").replace(/\/ws$/, "");
const idleMs = Number(idleArg) || 0;
const challengeMs = Number(challengeArg) || 30000;
const openingStamina = staminaArg === undefined ? 35 : Number(staminaArg);
const awayMs = Number(awayArg) || 30000;
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
    client.name = name;
    const hello = { t: "hello", v: 1, id: client.id, name, star: extra.star ?? 3, ver: "test", pvp: extra.pvp ?? true };
    if (extra.admin !== undefined) hello.admin = extra.admin;
    client.send(hello);
    client.welcome = await client.take("welcome");
    if (room !== undefined) {
      client.send({ t: "room", room });
      await client.take("peers");
      await client.take("occupancy");
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
  b.send({ t: "room", room });
  const peers = await b.take("peers");
  check("peers lists a", peers?.room === room && peers.list.length === 1 && peers.list[0].name === "Alice");
  check("peers entry fields", peers?.list[0]?.models.join() === "1,2,3" && peers.list[0].star === 3 && peers.list[0].pres?.x === 10);
  check("peers pid", peers?.list[0]?.pid === a.welcome.pid);
  check("occupancy with peers", (await b.take("occupancy"))?.others === 1);

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

async function testNoHistory() {
  const room = `hist_${run}`;
  const speaker = await Client.join("S", room);
  speaker.send({ t: "chat", scope: "room", text: "before" });
  speaker.send({ t: "chat", scope: "global", text: "before" });
  await speaker.take("chat");
  await speaker.take("chat");
  const probe = await Client.open();
  probe.send({ t: "hello", v: 1, id: randomId(), name: "P", star: 0, ver: "x" });
  await probe.take("welcome");
  probe.send({ t: "room", room });
  await probe.take("peers");
  check("no chat history sent", await probe.quiet(500, "history") && await probe.quiet(10, "chat"));
  probe.close();
  speaker.close();
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

// Closes `clients` and waits until the server has let them go: one address may hold only 4 sockets.
async function done(...clients) {
  for (const client of clients) client.close();
  for (const client of clients) await client.waitClosed();
  await sleep(100);
}

const card = (name, extra = {}) => ({ star: 3, members: [{ char: 0, name, hp: 120, atk: 14, def: 11, mag: 9, mdef: 8, elements: 6, ...extra }] });

// A challenge from `from` to `to`, taken by the target.
async function challenge(from, to, extra = {}) {
  from.send({ t: "challenge", to: to.welcome.pid, card: card(from.name), ver: "test", data: "", ...extra });
  return to.take("challenge");
}

async function testPvpFlag() {
  const room = `pvp_${run}`;
  const a = await Client.join("A", room);
  const b = await Client.join("B", room, { pvp: false });
  check("join carries pvp", (await a.take("join"))?.pvp === false);
  const cal = await Client.open();
  cal.send({ t: "hello", v: 1, id: randomId(), name: "C", star: 1, ver: "x" });
  await cal.take("welcome");
  cal.send({ t: "room", room });
  const peers = await cal.take("peers");
  check("peers carry pvp", peers?.list.find((entry) => entry.name === "A")?.pvp === true && peers.list.find((entry) => entry.name === "B")?.pvp === false);
  check("pvp off without the member", (await a.take("join", 2000, (message) => message.name === "C"))?.pvp === false);
  b.send({ t: "profile", name: "B", star: 2, pvp: true });
  check("profile carries pvp", (await a.take("profile"))?.pvp === true);
  b.send({ t: "profile", name: "B", star: 4 });
  check("profile without pvp keeps it", (await a.take("profile"))?.pvp === true);
  await done(a, b, cal);
}

async function testPm() {
  const a = await Client.join("Ann", `pm_${run}`);
  const b = await Client.join("Ben", `pm2_${run}`);
  const cal = await Client.join("Cy", `pm_${run}`);
  a.send({ t: "pm", to: b.welcome.pid, text: "  psst\u0007 " });
  const got = await b.take("chat");
  check(
    "pm reaches its player in any room",
    got?.scope === "pm" && got.text === "psst" && got.pid === a.welcome.pid && got.name === "Ann" && got.to === b.welcome.pid && got.to_name === "Ben" && got.ts > 1e12,
    JSON.stringify(got),
  );
  const echo = await a.take("chat");
  check("pm echoed to the sender", echo?.scope === "pm" && echo.to === b.welcome.pid && echo.to_name === "Ben");
  check("pm reaches nobody else", await cal.quiet(300, "chat"));
  a.send({ t: "pm", to: "0123456789abcdef", text: "anyone?" });
  const offline = await a.take("error");
  check("pm to a player not online", offline?.code === "offline" && offline.pid === "0123456789abcdef");
  a.send({ t: "pm", to: "not a pid", text: "x" });
  check("pm with a bad pid", (await a.take("error"))?.code === "bad");
  a.send({ t: "pm", to: a.welcome.pid, text: "me" });
  check("pm to oneself refused", (await a.take("error"))?.code === "bad");
  a.send({ t: "pm", to: b.welcome.pid, text: " \u0001 " });
  check("empty pm dropped", await b.quiet(300, "chat"));
  await done(cal);

  const probe = await Client.open();
  probe.send({ t: "hello", v: 1, id: randomId(), name: "P", star: 0, ver: "x" });
  await probe.take("welcome");
  check("pm not replayed to a newcomer", await probe.quiet(300, "chat"));
  await done(probe);

  const fast = await Client.join("Fast", undefined);
  for (let i = 0; i < 3; i++) fast.send({ t: "chat", scope: "global", text: `c${i}` });
  for (let i = 0; i < 3; i++) fast.send({ t: "pm", to: b.welcome.pid, text: `p${i}` });
  check("pm shares the chat rate limit", (await fast.take("error"))?.code === "rate");
  await done(a, b, fast);
}

async function testChallengeRefusals() {
  const a = await Client.join("A", undefined);
  const b = await Client.join("B", undefined, { star: 12 });
  const off = await Client.join("Off", undefined, { pvp: false });
  const got = await challenge(a, b, { card: { star: 5000, members: [{ char: 999, name: "  Serge\u0001  ", hp: 99999, atk: -5, def: 3, mag: 4, mdef: 5, elements: 300, x: 1 }] } });
  const member = got?.card?.members?.[0];
  check("challenge forwarded", got?.from === a.welcome.pid && got.name === "A" && got.star === 3 && got.expires > 0 && got.expires <= challengeMs, JSON.stringify(got));
  check("challenge card clamped", got?.card.star === 999 && member?.char === 255 && member.name === "Serge" && member.hp === 9999 && member.atk === 0 && member.elements === 99 && member.x === undefined);
  b.send({ t: "challenge_reply", from: a.welcome.pid, accept: false, reason: "declined" });
  check("decline ends it for the challenger", (await a.take("challenge_end"))?.reason === "declined");
  check("the decliner gets no end", await b.quiet(300, "challenge_end"));
  a.send({ t: "challenge", to: b.welcome.pid, card: card("A"), ver: "test", data: "" });
  const cool = await a.take("challenge_end");
  check("same pair within 10 s: cooldown", cool?.reason === "cooldown" && cool.pid === b.welcome.pid);
  check("cooldown not forwarded", await b.quiet(300, "challenge"));

  a.send({ t: "challenge", to: off.welcome.pid, card: card("A"), ver: "test", data: "" });
  check("target with pvp off", (await a.take("challenge_end"))?.reason === "off" && (await off.quiet(300, "challenge")));
  off.send({ t: "challenge", to: b.welcome.pid, card: card("Off"), ver: "test", data: "" });
  check("challenger with pvp off", (await off.take("challenge_end"))?.reason === "off");
  a.send({ t: "challenge", to: "0123456789abcdef", card: card("A"), ver: "test", data: "" });
  check("target not connected", (await a.take("challenge_end"))?.reason === "left");
  a.send({ t: "challenge", to: a.welcome.pid, card: card("A"), ver: "test", data: "" });
  check("challenging oneself", (await a.take("error"))?.code === "bad");
  a.send({ t: "challenge", to: b.welcome.pid, card: { star: 1, members: [{}, {}, {}, {}] }, ver: "test", data: "" });
  check("bad card", (await a.take("error"))?.code === "bad");
  a.send({ t: "challenge", to: b.welcome.pid, card: card("A"), ver: "test", data: "XYZ" });
  check("bad data", (await a.take("error"))?.code === "bad");
  await done(a, b, off);

  const cal = await Client.join("C", undefined);
  const dee = await Client.join("D", undefined);
  const eve = await Client.join("E", undefined);
  check("pending challenge forwarded", (await challenge(cal, dee)) !== null);
  cal.send({ t: "challenge", to: eve.welcome.pid, card: card("C"), ver: "test", data: "" });
  check("one outgoing at a time", (await cal.take("challenge_end"))?.reason === "pending" && (await eve.quiet(300, "challenge")));
  eve.send({ t: "challenge", to: dee.welcome.pid, card: card("E"), ver: "test", data: "" });
  check("target with a pending challenge is busy", (await eve.take("challenge_end"))?.reason === "busy");
  eve.send({ t: "challenge", to: cal.welcome.pid, card: card("E"), ver: "test", data: "" });
  check("challenger with a pending challenge is busy", (await eve.take("challenge_end"))?.reason === "busy");
  dee.send({ t: "challenge", to: eve.welcome.pid, card: card("D"), ver: "test", data: "" });
  check("a challenged player cannot challenge", (await dee.take("challenge_end"))?.reason === "pending");
  cal.send({ t: "challenge_cancel" });
  const cancelled = await dee.take("challenge_end");
  check("cancel reaches the target", cancelled?.reason === "cancelled" && cancelled.pid === cal.welcome.pid);
  check("canceller gets no end", await cal.quiet(300, "challenge_end"));
  check("free again after a cancel", (await challenge(dee, eve)) !== null);
  eve.send({ t: "challenge_reply", from: dee.welcome.pid, accept: false, reason: "busy" });
  check("busy reply passed on", (await dee.take("challenge_end"))?.reason === "busy");
  check("free again after a reply", (await challenge(cal, eve)) !== null);
  eve.send({ t: "challenge_reply", from: cal.welcome.pid, accept: false, reason: "bogus" });
  check("unknown reason reads as declined", (await cal.take("challenge_end"))?.reason === "declined");
  eve.send({ t: "challenge_reply", from: cal.welcome.pid, accept: false, reason: "off" });
  check("reply to no challenge ignored", (await cal.quiet(300, "challenge_end")) && (await eve.quiet(0, "error")));
  await done(cal, dee, eve);
}

async function testChallengeTimeAndLeave() {
  const a = await Client.join("A", undefined);
  const b = await Client.join("B", undefined);
  await challenge(a, b);
  const start = Date.now();
  const late = await a.take("challenge_end", challengeMs + 4000);
  const waited = Date.now() - start;
  check("challenge times out", late?.reason === "timeout" && waited >= challengeMs - 500, `${late?.reason} after ${waited} ms`);
  check("timeout told to the target", (await b.take("challenge_end"))?.reason === "timeout");
  b.send({ t: "challenge_reply", from: a.welcome.pid, accept: true, card: card("B"), ver: "test", data: "" });
  check("accept after the timeout ignored", await a.quiet(300, "pvp_start"));
  await done(a, b);

  const cal = await Client.join("C", undefined);
  const dee = await Client.join("D", undefined);
  await challenge(cal, dee);
  cal.close();
  const left = await dee.take("challenge_end");
  check("challenger leaving ends it", left?.reason === "left" && left.pid === cal.welcome.pid);
  const eve = await Client.join("E", undefined);
  const fay = await Client.join("F", undefined);
  await challenge(eve, fay);
  fay.close();
  check("target leaving ends it", (await eve.take("challenge_end"))?.reason === "left");
  await done(dee, eve);
}

async function testChallengeAccept() {
  const a = await Client.join("A", undefined, { star: 4 });
  const b = await Client.join("B", undefined, { star: 9 });
  await challenge(a, b, { ver: "1.0" });
  b.send({ t: "challenge_reply", from: a.welcome.pid, accept: true, card: card("B"), ver: "1.1", data: "" });
  check("version mismatch to both", (await a.take("challenge_end"))?.reason === "version" && (await b.take("challenge_end"))?.reason === "version");
  await done(a, b);

  const cal = await Client.join("C", undefined);
  const dee = await Client.join("D", undefined);
  await challenge(cal, dee, { data: "aa11" });
  dee.send({ t: "challenge_reply", from: cal.welcome.pid, accept: true, card: card("D"), ver: "test", data: "bb22" });
  check("data mismatch to both", (await cal.take("challenge_end"))?.reason === "data" && (await dee.take("challenge_end"))?.reason === "data");
  await done(cal, dee);

  const eve = await Client.join("E", undefined, { star: 4 });
  const fay = await Client.join("F", undefined, { star: 9 });
  await challenge(eve, fay, { data: "aa11" });
  fay.send({ t: "challenge_reply", from: eve.welcome.pid, accept: true, card: card("Fcard", { hp: 200 }), ver: "test", data: "" });
  const ours = await eve.take("pvp_start");
  const theirs = await fay.take("pvp_start");
  check("pvp_start to both", ours !== null && theirs !== null);
  check("same battle, seed and first side", ours?.battle === theirs?.battle && /^[0-9a-fay]{16}$/.test(ours?.battle) && ours.seed === theirs.seed && ours.first === theirs.first);
  check("seed is 32-bit unsigned", Number.isInteger(ours?.seed) && ours.seed >= 0 && ours.seed <= 0xffffffff);
  check("first is one of the two", [eve.welcome.pid, fay.welcome.pid].includes(ours?.first));
  check("each gets the other", ours?.opponent === fay.welcome.pid && ours.name === "F" && ours.star === 9 && theirs?.opponent === eve.welcome.pid && theirs.name === "E" && theirs.star === 4);
  check("each gets the other's card", ours?.card.members[0].name === "Fcard" && ours.card.members[0].hp === 200 && theirs?.card.members[0].name === "E");
  check("opening stamina", ours?.opening_stamina === openingStamina && theirs?.opening_stamina === openingStamina, String(ours?.opening_stamina));

  const gil = await Client.join("G", undefined);
  gil.send({ t: "challenge", to: eve.welcome.pid, card: card("G"), ver: "test", data: "" });
  check("a player in a battle is busy", (await gil.take("challenge_end"))?.reason === "busy");
  eve.send({ t: "challenge", to: gil.welcome.pid, card: card("E"), ver: "test", data: "" });
  check("a player in a battle cannot challenge", (await eve.take("challenge_end"))?.reason === "busy");
  eve.send({ t: "pvp_end", battle: "0000000000000000", result: "won" });
  eve.send({ t: "pvp_end", battle: ours?.battle, result: "nope" });
  check("pvp_end with a bad result", (await eve.take("error"))?.code === "bad");
  gil.send({ t: "challenge", to: fay.welcome.pid, card: card("G"), ver: "test", data: "" });
  check("another battle's pvp_end changes nothing", (await gil.take("challenge_end"))?.reason === "busy");
  eve.send({ t: "pvp_end", battle: ours?.battle, result: "won" });
  await sleep(100);
  check("pvp_end frees its sender", (await challenge(gil, eve)) !== null);
  gil.send({ t: "challenge_cancel" });
  await eve.take("challenge_end");
  check("a won pvp_end tells the opponent nothing", await fay.quiet(200, "pvp_gone"));
  gil.send({ t: "challenge", to: fay.welcome.pid, card: card("G"), ver: "test", data: "" });
  check("the opponent stays in the battle until its own pvp_end", (await gil.take("challenge_end"))?.reason === "busy");
  fay.send({ t: "pvp_end", battle: theirs?.battle, result: "lost" });
  await sleep(100);
  check("both pvp_ends free both", (await challenge(gil, fay)) !== null);
  gil.send({ t: "challenge_cancel" });
  await fay.take("challenge_end");
  await done(gil, eve, fay);
}

// Two players in a battle: [a, b, start of a, start of b].
async function battlePair(nameA, nameB, extraA = {}, extraB = {}) {
  const a = await Client.join(nameA, undefined, extraA);
  const b = await Client.join(nameB, undefined, extraB);
  await challenge(a, b);
  b.send({ t: "challenge_reply", from: a.welcome.pid, accept: true, card: card(nameB), ver: "test", data: "" });
  return [a, b, await a.take("pvp_start"), await b.take("pvp_start")];
}

const hash = (value) => value.toString(16).padStart(16, "0");

// Section 8.4: the side exchange, the command and state check relay in order with relay numbers, and the checks on every battle message.
async function testBattleRelay() {
  const [a, b, start] = await battlePair("LinkA", "LinkB");
  const battle = start?.battle;
  check("battle pair started", typeof battle === "string");
  a.send({ t: "pvp_side", battle, side: "QUJD" });
  const side = await b.take("pvp_side");
  check("side relayed with relay number 1", side?.side === "QUJD" && side.battle === battle && side.n === 1, JSON.stringify(side));
  check("the sender gets no echo", await a.quiet(200, "pvp_side"));
  a.send({ t: "pvp_side", battle, side: "QUJD" });
  check("a second side refused", (await a.take("error"))?.code === "bad");
  const big = "A".repeat(10000);
  b.send({ t: "pvp_side", battle, side: big });
  check("a side over the 4096 frame limit is relayed", (await a.take("pvp_side"))?.side === big);
  b.send({ t: "pvp_hash", battle, seq: 1, hash: hash(1), pad: "x".repeat(5000) });
  check("other battle frames keep the 4096 limit", (await b.take("error"))?.code === "bad" && (await a.quiet(200, "pvp_hash")));
  for (let seq = 1; seq <= 5; seq++) a.send({ t: "pvp_cmd", battle, seq, cmd: `0 ${seq} 3 -1 -1 0 -1 0`, hash: hash(seq), ack: 1 });
  const got = [];
  for (let i = 0; i < 5; i++) got.push(await b.take("pvp_cmd"));
  check("commands relayed in order", got.every((m, i) => m?.seq === i + 1 && m.cmd === `0 ${i + 1} 3 -1 -1 0 -1 0` && m.hash === hash(i + 1)));
  check("relay numbers count up per recipient", got.every((m, i) => m?.n === i + 2), got.map((m) => m?.n).join());
  for (let seq = 1; seq <= 5; seq++) b.send({ t: "pvp_hash", battle, seq, hash: hash(seq), ack: seq + 1 });
  const hashes = [];
  for (let i = 0; i < 5; i++) hashes.push(await a.take("pvp_hash"));
  check("state checks relayed in order", hashes.every((m, i) => m?.seq === i + 1 && m.hash === hash(i + 1) && m.n === i + 2));
  a.send({ t: "pvp_cmd", battle, seq: 9, cmd: "5 0 -1 -1 -1 0 -1", hash: hash(9) });
  check("a seq that skips is refused", (await a.take("error"))?.code === "bad" && (await b.quiet(200, "pvp_cmd")));
  a.send({ t: "pvp_cmd", battle, seq: 5, cmd: "5 0", hash: hash(5) });
  check("a repeated seq is refused", (await a.take("error"))?.code === "bad");
  a.send({ t: "pvp_cmd", battle, seq: 6, cmd: "attack 0", hash: hash(6) });
  check("a command that is not numbers is refused", (await a.take("error"))?.code === "bad");
  a.send({ t: "pvp_cmd", battle, seq: 6, cmd: "5 0", hash: "XYZ" });
  check("a bad hash is refused", (await a.take("error"))?.code === "bad");
  a.send({ t: "pvp_cmd", battle: "0123456789abcdef", seq: 6, cmd: "5 0", hash: hash(6) });
  check("another battle's id is refused", (await a.take("error"))?.code === "bad");
  const cal = await Client.join("LinkC", undefined);
  cal.send({ t: "pvp_cmd", battle, seq: 1, cmd: "5 0", hash: hash(1) });
  cal.send({ t: "pvp_side", battle, side: "QUJD" });
  check("a player outside the battle is refused", (await cal.take("error"))?.code === "bad" && (await b.quiet(300, "pvp_cmd")) && (await b.quiet(0, "pvp_side")));
  a.send({ t: "pvp_cmd", battle, seq: 6, cmd: "5 0 -1 -1 -1 1 -1", hash: hash(6) });
  check("the stream goes on after refusals", (await b.take("pvp_cmd"))?.seq === 6);
  a.send({ t: "pvp_end", battle, result: "desync" });
  check("a desync pvp_end tells the opponent", (await b.take("pvp_gone"))?.reason === "aborted");
  b.send({ t: "pvp_end", battle, result: "aborted" });
  check("an aborted pvp_end is not sent back to a player who left", await a.quiet(200, "pvp_gone"));
  await done(a, b, cal);
}

// A member who disconnects: the opponent hears it is away, the messages meanwhile wait in its outbox, and a resume within the window gets
// the link counters and every message it has not acknowledged, in order; the opponent hears it is back.
async function testBattleResume() {
  const id = randomId();
  const [a, b, start] = await battlePair("AwayA", "AwayB", {}, { id });
  const battle = start?.battle;
  b.send({ t: "pvp_side", battle, side: "QkJC" });
  await a.take("pvp_side");
  a.send({ t: "pvp_cmd", battle, seq: 1, cmd: "6 0", hash: hash(1) });
  check("first command arrives", (await b.take("pvp_cmd"))?.n === 1);
  b.send({ t: "pvp_hash", battle, seq: 1, hash: hash(1), ack: 1 });
  await a.take("pvp_hash");
  a.send({ t: "pvp_cmd", battle, seq: 2, cmd: "0 0 3 -1 -1 0 -1 1", hash: hash(2) });
  await b.take("pvp_cmd");
  b.close();
  await b.waitClosed();
  const away = await a.take("pvp_away");
  check("the opponent is told the player is away", away?.battle === battle && away.ms === awayMs, JSON.stringify(away));
  a.send({ t: "pvp_cmd", battle, seq: 3, cmd: "5 0", hash: hash(3) });
  a.send({ t: "pvp_cmd", battle, seq: 4, cmd: "6 3", hash: hash(4) });
  await sleep(200);
  const back = await Client.join("AwayB", undefined, { id });
  back.send({ t: "pvp_resume", battle, ack: 1 });
  const resumed = await back.take("pvp_resumed");
  check("resume gives the link counters", resumed?.battle === battle && resumed.seq === 1 && resumed.side === true, JSON.stringify(resumed));
  const missed = [];
  for (let i = 0; i < 3; i++) missed.push(await back.take("pvp_cmd"));
  check(
    "resume delivers the unacknowledged messages in order",
    missed.map((m) => m?.seq).join() === "2,3,4" && missed.map((m) => m?.n).join() === "2,3,4",
    missed.map((m) => `${m?.seq}/${m?.n}`).join(),
  );
  check("the opponent hears the player is back", (await a.take("pvp_back"))?.battle === battle);
  back.send({ t: "pvp_hash", battle, seq: 2, hash: hash(2), ack: 4 });
  check("the stream goes on after the resume", (await a.take("pvp_hash"))?.seq === 2);
  a.send({ t: "pvp_cmd", battle, seq: 5, cmd: "5 0", hash: hash(5) });
  check("relay numbers go on after the resume", (await back.take("pvp_cmd"))?.n === 5);

  const second = await Client.join("AwayB", undefined, { id });
  second.send({ t: "pvp_resume", battle, ack: 4 });
  const taken = await second.take("pvp_resumed");
  check("a second socket of the same player takes the battle over", taken?.seq === 2 && (await back.waitClosed()));
  check("and gets what the old socket had not acknowledged", (await second.take("pvp_cmd"))?.seq === 5);
  a.send({ t: "pvp_cmd", battle, seq: 6, cmd: "5 0", hash: hash(6) });
  check("relays go to the new socket", (await second.take("pvp_cmd"))?.seq === 6);
  check("the takeover is no disconnect", await a.quiet(300, "pvp_away"));
  const stranger = await Client.join("Stranger", undefined);
  stranger.send({ t: "pvp_resume", battle, ack: 0 });
  check("a resume by a player who never held the battle is refused", (await stranger.take("pvp_gone"))?.reason === "closed");
  a.send({ t: "pvp_end", battle, result: "aborted" });
  second.send({ t: "pvp_end", battle, result: "aborted" });
  await done(a, second, stranger);
}

// A member who stays away for the window forfeits: the opponent wins (pvp_gone left) and is free, a late resume hears it lost. When both
// members are gone the battle is dropped.
async function testBattleForfeit() {
  const id = randomId();
  const [a, b, start] = await battlePair("StayA", "StayB", {}, { id });
  const battle = start?.battle;
  b.close();
  await b.waitClosed();
  await a.take("pvp_away");
  const begin = Date.now();
  const gone = await a.take("pvp_gone", awayMs + 4000);
  const waited = Date.now() - begin;
  check("the opponent wins after the away time", gone?.reason === "left" && gone.battle === battle && waited >= awayMs - 500, `${gone?.reason} after ${waited} ms`);
  const late = await Client.join("StayB", undefined, { id });
  late.send({ t: "pvp_resume", battle, ack: 0 });
  check("a late resume hears it forfeited", (await late.take("pvp_gone"))?.reason === "forfeit");
  check("the winner is free after a forfeit", (await challenge(late, a)) !== null);
  late.send({ t: "challenge_cancel" });
  await a.take("challenge_end");
  await done(a, late);

  const idC = randomId();
  const [cal, dee, other] = await battlePair("BothC", "BothD", { id: idC });
  cal.close();
  await cal.waitClosed();
  await dee.take("pvp_away");
  dee.close();
  await dee.waitClosed();
  const back = await Client.join("BothC", undefined, { id: idC });
  back.send({ t: "pvp_resume", battle: other?.battle, ack: 0 });
  check("when both members left the battle is gone", (await back.take("pvp_gone"))?.reason === "closed");
  await done(back);
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

const tests = [
  testHandshake, testRooms, testChat, testNoHistory, testPresRate, testAdmin, testAdminChat, testPresClamp, testPvpFlag, testPm, testChallengeRefusals,
  testChallengeTimeAndLeave, testChallengeAccept, testBattleRelay, testBattleResume, testBattleForfeit, testIdle, testAuthLockout,
];
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
