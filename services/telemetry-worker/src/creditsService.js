// Credits routes and jobs on the telemetry Worker. Storage is the CREDITS_DB D1 binding (schema.sql).
// Secrets: ADMIN_TOKEN, KOFI_TOKEN, GITHUB_TOKEN, STEAM_KEY (publisher key), SAFE_BROWSING_KEY (optional).
// Alerts go to the alerts table, and by email when the ALERT_EMAIL send_email binding and ALERT_TO are set.

import {
  TIERS,
  applyVerdict,
  buildCredits,
  canSetLink,
  checkLinkUrl,
  DEFAULT_ALLOWLIST,
  judgeLink,
  parseKofi,
  personTier,
  tierRank,
} from "./credits.js";

const STEAM_APP = "5520720";
const MAX_REDIRECTS = 5;
const MAX_REPORTS_PER_DAY = 200;
const SETTING_KEYS = new Set(["corporate", "pastSponsors", "contributors", "testers", "hide", "allowlist", "fx", "steamDlc", "founderOpen"]);

const json = (body, status = 200, headers = {}) =>
  new Response(JSON.stringify(body), { status, headers: { "Content-Type": "application/json", ...headers } });

async function all(db, sql, ...args) {
  return (await db.prepare(sql).bind(...args).all()).results || [];
}

async function first(db, sql, ...args) {
  return db.prepare(sql).bind(...args).first();
}

async function run(db, sql, ...args) {
  return db.prepare(sql).bind(...args).run();
}

export async function getSetting(db, key, fallback) {
  const row = await first(db, "SELECT value FROM settings WHERE key = ?", key);
  if (!row) return fallback;
  try {
    return JSON.parse(row.value);
  } catch {
    return fallback;
  }
}

async function setSetting(db, key, value) {
  await run(db, "INSERT INTO settings (key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value", key, JSON.stringify(value));
}

async function sha256(text) {
  const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(text));
  return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

export async function alert(env, message, now = Date.now()) {
  await run(env.CREDITS_DB, "INSERT INTO alerts (at, message) VALUES (?, ?)", new Date(now).toISOString(), message);
  if (!env.ALERT_EMAIL || !env.ALERT_TO) return;
  try {
    const { EmailMessage } = await import("cloudflare:email");
    const from = env.ALERT_FROM || `credits@${env.ALERT_DOMAIN || "lila3d.workers.dev"}`;
    const raw = `From: ${from}\r\nTo: ${env.ALERT_TO}\r\nSubject: Lila credits: ${message.slice(0, 80)}\r\nContent-Type: text/plain; charset=utf-8\r\n\r\n${message}\r\n`;
    await env.ALERT_EMAIL.send(new EmailMessage(from, env.ALERT_TO, raw));
  } catch (e) {
    console.log(`alert email failed: ${e}`);
  }
}

async function upsertDonor(db, id, name, isPublic, now) {
  await run(
    db,
    `INSERT INTO donors (id, person_id, name, public, updated_at) VALUES (?, ?, ?, ?, ?)
     ON CONFLICT(id) DO UPDATE SET name = excluded.name, public = excluded.public, updated_at = excluded.updated_at`,
    id, id, name || null, isPublic ? 1 : 0, new Date(now).toISOString());
}

/** Works out every person's tier from their payments, numbering new Legendaries and marking Founders. */
export async function recompute(env, now = Date.now()) {
  const db = env.CREDITS_DB;
  const donors = await all(db, "SELECT * FROM donors");
  const payments = await all(db, "SELECT p.*, d.person_id FROM payments p JOIN donors d ON d.id = p.donor_id");
  const people = Object.fromEntries((await all(db, "SELECT * FROM people")).map((p) => [p.id, p]));
  const founderOpen = await getSetting(db, "founderOpen", false);
  let nextNumber = 1 + Math.max(0, ...Object.values(people).map((p) => p.number || 0));

  const byPerson = {};
  for (const d of donors) byPerson[d.person_id] ??= [];
  for (const p of payments) (byPerson[p.person_id] ??= []).push(p);

  for (const [id, list] of Object.entries(byPerson)) {
    const { tier } = personTier(list, now);
    const row = people[id] || { id, name: null, hidden: 0, number: null, founder: 0 };
    let number = row.number;
    if (!number && tierRank(tier) >= tierRank("legendary")) number = nextNumber++;
    const founder = row.founder || (founderOpen && tierRank(tier) >= tierRank("supporter")) ? 1 : 0;
    await run(
      db,
      `INSERT INTO people (id, name, hidden, tier, number, founder) VALUES (?, ?, ?, ?, ?, ?)
       ON CONFLICT(id) DO UPDATE SET tier = excluded.tier, number = excluded.number, founder = excluded.founder`,
      id, row.name, row.hidden || 0, tier, number, founder);
  }
}

/** Rebuilds credits.json from the database and stores it for GET /credits.json. */
export async function publish(env, now = Date.now()) {
  const db = env.CREDITS_DB;
  const donors = await all(db, "SELECT * FROM donors ORDER BY updated_at DESC");
  const people = (await all(db, "SELECT * FROM people")).map((p) => {
    const own = donors.filter((d) => d.person_id === p.id);
    return {
      id: p.id,
      name: p.name || own.find((d) => d.public && d.name)?.name || "",
      public: own.some((d) => d.public),
      hidden: !!p.hidden,
      tier: p.tier,
      number: p.number,
      founder: !!p.founder,
    };
  });
  const links = Object.fromEntries((await all(db, "SELECT * FROM links")).map((l) => [l.owner, l]));
  const settings = {};
  for (const key of ["corporate", "pastSponsors", "contributors", "testers", "hide"]) settings[key] = await getSetting(db, key, []);
  const credits = buildCredits({ people, links, settings, now });
  const body = JSON.stringify(credits);
  await run(
    db,
    "INSERT INTO published (key, body, updated) VALUES ('credits', ?, ?) ON CONFLICT(key) DO UPDATE SET body = excluded.body, updated = excluded.updated",
    body, credits.updated);
  return credits;
}

async function rebuild(env, now) {
  await recompute(env, now);
  return publish(env, now);
}

// ---- GitHub Sponsors ----

async function github(env, query, variables) {
  const res = await fetch("https://api.github.com/graphql", {
    method: "POST",
    headers: { Authorization: `Bearer ${env.GITHUB_TOKEN}`, "Content-Type": "application/json", "User-Agent": "lila-credits" },
    body: JSON.stringify({ query, variables }),
  });
  const body = await res.json();
  if (!res.ok || body.errors) throw new Error(`GitHub: ${JSON.stringify(body.errors || res.status)}`);
  return body.data;
}

const SPONSOR = "sponsor: sponsorEntity { __typename ... on User { login name } ... on Organization { login name } }";

/**
 * Pulls lifetime totals and privacy from GitHub Sponsors. Growth since the last poll is stored as a new payment dated
 * now, so the chargeback window applies to new money and not to the sponsor's first date.
 */
export async function pollGitHub(env, now = Date.now()) {
  if (!env.GITHUB_TOKEN) return;
  const db = env.CREDITS_DB;
  const sponsors = {};
  for (let after = null; ;) {
    const data = await github(env, `query($after: String) { viewer { sponsorshipsAsMaintainer(first: 100, after: $after, activeOnly: false, includePrivate: true) {
      pageInfo { hasNextPage endCursor } nodes { createdAt privacyLevel ${SPONSOR} } } } }`, { after });
    const page = data.viewer.sponsorshipsAsMaintainer;
    for (const n of page.nodes) {
      if (!n.sponsor?.login) continue;
      const s = (sponsors[n.sponsor.login] ??= { name: n.sponsor.name || n.sponsor.login, public: false, since: n.createdAt });
      s.public ||= n.privacyLevel === "PUBLIC";
      if (n.createdAt < s.since) s.since = n.createdAt;
    }
    if (!page.pageInfo.hasNextPage) break;
    after = page.pageInfo.endCursor;
  }
  for (let after = null; ;) {
    const data = await github(env, `query($after: String) { viewer { lifetimeReceivedSponsorshipValues(first: 100, after: $after) {
      pageInfo { hasNextPage endCursor } nodes { amountInCents sponsor { __typename ... on User { login name } ... on Organization { login name } } } } } }`, { after });
    const page = data.viewer.lifetimeReceivedSponsorshipValues;
    for (const n of page.nodes) {
      if (!n.sponsor?.login) continue;
      (sponsors[n.sponsor.login] ??= { name: n.sponsor.name || n.sponsor.login, public: false, since: new Date(now).toISOString() }).lifetime = n.amountInCents;
    }
    if (!page.pageInfo.hasNextPage) break;
    after = page.pageInfo.endCursor;
  }

  for (const [login, s] of Object.entries(sponsors)) {
    const donor = `github:${login}`;
    await upsertDonor(db, donor, s.name, s.public, now);
    if (!Number.isFinite(s.lifetime)) continue;
    const have = await first(db, "SELECT COALESCE(SUM(cents), 0) AS total, COUNT(*) AS n FROM payments WHERE donor_id = ?", donor);
    const diff = s.lifetime - (have?.total || 0);
    if (diff === 0) continue;
    const at = have?.n ? new Date(now).toISOString() : s.since;
    await run(db, "INSERT OR IGNORE INTO payments (id, donor_id, cents, currency, at) VALUES (?, ?, ?, 'USD', ?)",
      `${donor}:${new Date(now).toISOString()}`, donor, diff, at);
  }
}

// ---- Link checks ----

async function fetchChain(url) {
  const hops = [];
  let next = url;
  for (let i = 0; i <= MAX_REDIRECTS; ++i) {
    let res;
    try {
      res = await fetch(next, { redirect: "manual", headers: { "User-Agent": "LilaCreditsLinkCheck/1 (+https://lila-3d.github.io)" } });
    } catch (e) {
      hops.push({ url: next, error: String(e) });
      return hops;
    }
    hops.push({ url: next, status: res.status });
    const location = res.headers.get("Location");
    if (res.status < 300 || res.status >= 400 || !location) return hops;
    next = new URL(location, next).toString();
  }
  hops.push({ url: next, status: 310 });
  return hops;
}

async function safeBrowsingFlags(env, urls) {
  if (!env.SAFE_BROWSING_KEY || urls.length === 0) return new Set();
  const res = await fetch(`https://safebrowsing.googleapis.com/v4/threatMatches:find?key=${env.SAFE_BROWSING_KEY}`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      client: { clientId: "lila-credits", clientVersion: "1" },
      threatInfo: {
        threatTypes: ["MALWARE", "SOCIAL_ENGINEERING", "UNWANTED_SOFTWARE", "POTENTIALLY_HARMFUL_APPLICATION"],
        platformTypes: ["ANY_PLATFORM"],
        threatEntryTypes: ["URL"],
        threatEntries: urls.map((url) => ({ url })),
      },
    }),
  });
  if (!res.ok) return new Set();
  const body = await res.json();
  return new Set((body.matches || []).map((m) => m.threat?.url));
}

/** Checks every live link; anything that now leads elsewhere, is flagged or has been dead for 30 days is dropped. */
export async function checkLinks(env, now = Date.now(), fetchHops = fetchChain) {
  const db = env.CREDITS_DB;
  const links = await all(db, "SELECT * FROM links WHERE banned = 0 AND status IN ('approved', 'ok')");
  for (const link of links) {
    const hops = await fetchHops(link.url);
    const flags = await safeBrowsingFlags(env, hops.map((h) => h.url));
    const result = judgeLink(link.host, hops, hops.some((h) => flags.has(h.url)));
    const update = applyVerdict(link, result, now);
    await updateLink(db, link.owner, update);
    if (update.status && update.status !== link.status && update.status !== "ok")
      await alert(env, `Link for ${link.owner} was ${update.status === "dead" ? "dropped as dead" : "pulled"}: ${update.reason} (${link.url})`, now);
  }
}

async function updateLink(db, owner, fields) {
  const keys = Object.keys(fields);
  if (keys.length === 0) return;
  await run(db, `UPDATE links SET ${keys.map((k) => `${k} = ?`).join(", ")} WHERE owner = ?`, ...keys.map((k) => fields[k]), owner);
}

export async function nightly(env, now = Date.now()) {
  try {
    await pollGitHub(env, now);
  } catch (e) {
    await alert(env, `GitHub Sponsors poll failed: ${e.message || e}`, now);
  }
  await recompute(env, now);
  await checkLinks(env, now);
  await publish(env, now);
}

// ---- Public routes ----

async function servePublished(env) {
  const row = await first(env.CREDITS_DB, "SELECT body FROM published WHERE key = 'credits'");
  const body = row?.body || JSON.stringify({ version: 1, corporate: [], pastSponsors: [], supporters: [], contributors: [], testers: [] });
  return new Response(body, {
    headers: { "Content-Type": "application/json", "Cache-Control": "public, max-age=300", "Access-Control-Allow-Origin": "*" },
  });
}

async function report(request, env, now) {
  let body;
  try {
    body = await request.json();
  } catch {
    return json({ error: "bad json" }, 400);
  }
  const clip = (v, n) => String(v ?? "").slice(0, n);
  const today = new Date(now).toISOString().slice(0, 10);
  const count = await first(env.CREDITS_DB, "SELECT COUNT(*) AS n FROM reports WHERE at >= ?", today);
  if ((count?.n || 0) >= MAX_REPORTS_PER_DAY) return json({ error: "too many reports today" }, 429);
  await run(env.CREDITS_DB, "INSERT INTO reports (owner, name, url, reason, at) VALUES (?, ?, ?, ?, ?)",
    clip(body.owner, 200), clip(body.name, 200), clip(body.url, 2000), clip(body.reason, 2000), new Date(now).toISOString());
  await alert(env, `Link reported: ${clip(body.name, 200)} ${clip(body.url, 2000)}${body.reason ? ` (${clip(body.reason, 500)})` : ""}`, now);
  return json({ ok: true });
}

async function kofi(request, env, now) {
  const form = await request.formData();
  let data;
  try {
    data = JSON.parse(form.get("data"));
  } catch {
    return json({ error: "bad data" }, 400);
  }
  if (!env.KOFI_TOKEN || data?.verification_token !== env.KOFI_TOKEN) return json({ error: "bad token" }, 403);
  const fx = await getSetting(env.CREDITS_DB, "fx", { USD: 1 });
  const k = parseKofi(data, fx);
  if (!k) return json({ error: "no transaction" }, 400);
  const donor = `kofi:${k.email ? await sha256(k.email) : k.transactionId}`;
  await upsertDonor(env.CREDITS_DB, donor, k.name, k.public, now);
  await run(env.CREDITS_DB, "INSERT OR IGNORE INTO payments (id, donor_id, cents, currency, at) VALUES (?, ?, ?, ?, ?)",
    `kofi:${k.transactionId}`, donor, k.cents, k.currency, k.at);
  if (k.cents === null) await alert(env, `Ko-fi payment ${k.transactionId} in ${k.currency} has no exchange rate in settings.fx`, now);
  await rebuild(env, now);
  return json({ ok: true });
}

/** Steam supporter DLC: verifies the auth ticket, reads which tier DLC the account owns, and records the opt-in. */
async function steam(request, env, now) {
  let body;
  try {
    body = await request.json();
  } catch {
    return json({ error: "bad json" }, 400);
  }
  if (!env.STEAM_KEY) return json({ error: "steam not configured" }, 503);
  const auth = await fetch(`https://partner.steam-api.com/ISteamUserAuth/AuthenticateUserTicket/v1/?key=${env.STEAM_KEY}&appid=${STEAM_APP}&ticket=${encodeURIComponent(body.ticket || "")}&identity=lila-credits`);
  const params = (await auth.json())?.response?.params;
  if (!params || params.result !== "OK" || !params.steamid) return json({ error: "ticket rejected" }, 403);
  const steamid = params.steamid;

  let tier = null;
  for (const dlc of await getSetting(env.CREDITS_DB, "steamDlc", [])) {
    const res = await fetch(`https://partner.steam-api.com/ISteamUser/CheckAppOwnership/v2/?key=${env.STEAM_KEY}&steamid=${steamid}&appid=${dlc.appid}`);
    const owns = (await res.json())?.appownership?.ownsapp;
    if (owns && tierRank(dlc.tier) > tierRank(tier)) tier = dlc.tier;
  }
  const donor = `steam:${steamid}`;
  await upsertDonor(env.CREDITS_DB, donor, String(body.name || "").trim().slice(0, 64), !!body.public, now);
  const cents = tier ? TIERS.find((t) => t.id === tier).cents : 0;
  await run(env.CREDITS_DB,
    `INSERT INTO payments (id, donor_id, cents, currency, at) VALUES (?, ?, ?, 'USD', ?)
     ON CONFLICT(id) DO UPDATE SET cents = excluded.cents`,
    `${donor}:dlc`, donor, cents, new Date(now).toISOString());
  await rebuild(env, now);
  return json({ ok: true, tier });
}

// ---- Admin routes ----

function authorised(request, env) {
  const given = request.headers.get("Authorization") || "";
  const want = `Bearer ${env.ADMIN_TOKEN || ""}`;
  if (!env.ADMIN_TOKEN || given.length !== want.length) return false;
  let diff = 0;
  for (let i = 0; i < want.length; ++i) diff |= given.charCodeAt(i) ^ want.charCodeAt(i);
  return diff === 0;
}

/** Approves or replaces a link. Corporate owners may use any https site; everyone else an allowlisted profile. */
export async function setLink(env, owner, url, { force = false, now = Date.now() } = {}) {
  const db = env.CREDITS_DB;
  const allowlist = await getSetting(db, "allowlist", DEFAULT_ALLOWLIST);
  const checked = checkLinkUrl(url, { corporate: owner.startsWith("corporate:"), allowlist });
  if (!checked.ok) return { ok: false, reason: checked.reason };
  const existing = await first(db, "SELECT * FROM links WHERE owner = ?", owner);
  const allowed = canSetLink(existing, checked.url, now, force);
  if (!allowed.ok) return allowed;
  const at = new Date(now).toISOString();
  const changed = !existing || existing.url !== checked.url ? at : existing.changed_at;
  await run(db,
    `INSERT INTO links (owner, url, host, status, reason, approved_at, changed_at, strikes, banned) VALUES (?, ?, ?, 'approved', NULL, ?, ?, 0, 0)
     ON CONFLICT(owner) DO UPDATE SET url = excluded.url, host = excluded.host, status = 'approved', reason = NULL,
       approved_at = excluded.approved_at, changed_at = excluded.changed_at, dead_since = NULL`,
    owner, checked.url, checked.host, at, changed);
  return { ok: true };
}

/** Takes a link down at once. ban stops any link for the owner from being approved again. */
export async function pullLink(env, owner, { reason = "pulled by hand", ban = false, now = Date.now() } = {}) {
  const db = env.CREDITS_DB;
  const existing = await first(db, "SELECT * FROM links WHERE owner = ?", owner);
  if (!existing) return { ok: false, reason: "no link for that owner" };
  await updateLink(db, owner, { status: "pulled", reason, strikes: (existing.strikes || 0) + 1, banned: ban ? 1 : existing.banned });
  await publish(env, now);
  return { ok: true };
}

async function admin(request, env, path, now) {
  if (!authorised(request, env)) return json({ error: "unauthorised" }, 401);
  const db = env.CREDITS_DB;
  const body = request.method === "GET" ? {} : await request.json().catch(() => ({}));

  if (request.method === "GET" && path === "/admin/state") {
    const settings = Object.fromEntries((await all(db, "SELECT * FROM settings")).map((s) => [s.key, JSON.parse(s.value)]));
    return json({
      people: await all(db, "SELECT * FROM people"),
      donors: await all(db, "SELECT * FROM donors"),
      payments: await all(db, "SELECT * FROM payments ORDER BY at DESC LIMIT 500"),
      links: await all(db, "SELECT * FROM links"),
      settings,
      reports: await all(db, "SELECT * FROM reports ORDER BY id DESC LIMIT 100"),
      alerts: await all(db, "SELECT * FROM alerts ORDER BY id DESC LIMIT 100"),
    });
  }
  if (request.method === "PUT" && path.startsWith("/admin/settings/")) {
    const key = path.slice("/admin/settings/".length);
    if (!SETTING_KEYS.has(key)) return json({ error: `unknown setting ${key}` }, 400);
    await setSetting(db, key, body.value);
    await rebuild(env, now);
    return json({ ok: true });
  }
  if (request.method !== "POST") return json({ error: "not found" }, 404);

  switch (path) {
    case "/admin/person": {
      if (!body.id) return json({ error: "id required" }, 400);
      await run(db, "INSERT OR IGNORE INTO people (id, hidden, founder) VALUES (?, 0, 0)", body.id);
      if ("name" in body) await run(db, "UPDATE people SET name = ? WHERE id = ?", body.name || null, body.id);
      if ("hidden" in body) await run(db, "UPDATE people SET hidden = ? WHERE id = ?", body.hidden ? 1 : 0, body.id);
      for (const donor of body.merge || []) await run(db, "UPDATE donors SET person_id = ? WHERE id = ?", body.id, donor);
      await rebuild(env, now);
      return json({ ok: true });
    }
    case "/admin/link": {
      const result = await setLink(env, String(body.owner || ""), String(body.url || ""), { force: !!body.force, now });
      if (result.ok) await publish(env, now);
      return json(result, result.ok ? 200 : 400);
    }
    case "/admin/pull": {
      const result = await pullLink(env, String(body.owner || ""), { reason: body.reason, ban: !!body.ban, now });
      return json(result, result.ok ? 200 : 404);
    }
    case "/admin/payment": {
      if (!body.id) return json({ error: "id required" }, 400);
      if (body.donor_id) {
        if (!(await first(db, "SELECT id FROM donors WHERE id = ?", body.donor_id)))
          await upsertDonor(db, body.donor_id, body.name, !!body.public, now);
        await run(db, `INSERT INTO payments (id, donor_id, cents, currency, at, reversed) VALUES (?, ?, ?, 'USD', ?, ?)
          ON CONFLICT(id) DO UPDATE SET cents = excluded.cents, reversed = excluded.reversed`,
          body.id, body.donor_id, body.cents ?? null, body.at || new Date(now).toISOString(), body.reversed ? 1 : 0);
      } else {
        await run(db, "UPDATE payments SET reversed = ? WHERE id = ?", body.reversed ? 1 : 0, body.id);
      }
      await rebuild(env, now);
      return json({ ok: true });
    }
    case "/admin/publish":
      return json(await rebuild(env, now));
    case "/admin/nightly":
      await nightly(env, now);
      return json({ ok: true });
  }
  return json({ error: "not found" }, 404);
}

/** The credits routes, or null when the path isn't one of them. */
export async function handleCredits(request, env, now = Date.now()) {
  const path = new URL(request.url).pathname;
  if (!env.CREDITS_DB) return path === "/credits.json" || path.startsWith("/credits/") || path.startsWith("/admin/") ? json({ error: "credits not configured" }, 503) : null;
  if (path === "/credits.json" && request.method === "GET") return servePublished(env);
  if (path === "/credits/report" && request.method === "POST") return report(request, env, now);
  if (path === "/credits/kofi" && request.method === "POST") return kofi(request, env, now);
  if (path === "/credits/steam" && request.method === "POST") return steam(request, env, now);
  if (path.startsWith("/admin/")) return admin(request, env, path, now);
  return null;
}
