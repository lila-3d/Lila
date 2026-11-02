import test from "node:test";
import assert from "node:assert/strict";

import {
  applyVerdict,
  buildCredits,
  canSetLink,
  checkLinkUrl,
  DAY_MS,
  judgeLink,
  parseKofi,
  personTier,
  tierFor,
} from "../src/credits.js";
import { checkLinks, handleCredits, publish, recompute, setLink } from "../src/creditsService.js";
import { fakeD1 } from "./d1.js";

const NOW = Date.parse("2026-11-03T00:00:00Z");
const ago = (days) => new Date(NOW - days * DAY_MS).toISOString();

test("tiers start at their thresholds", () => {
  assert.equal(tierFor(999), null);
  assert.equal(tierFor(1000), "backer");
  assert.equal(tierFor(2999), "backer");
  assert.equal(tierFor(3000), "supporter");
  assert.equal(tierFor(10000), "patron");
  assert.equal(tierFor(50000), "legendary");
  assert.equal(tierFor(500000), "mythic");
  assert.equal(tierFor(5000000), "eternal");
});

test("Mythic and up wait for the chargeback window; reversals don't count", () => {
  assert.equal(personTier([{ cents: 600000, at: ago(10) }], NOW).tier, "legendary");
  assert.equal(personTier([{ cents: 600000, at: ago(200) }], NOW).tier, "mythic");
  assert.equal(personTier([{ cents: 600000, at: ago(200), reversed: 1 }], NOW).tier, null);
  assert.equal(personTier([{ cents: 40000, at: ago(1) }, { cents: 20000, at: ago(1) }], NOW).tier, "legendary");
  assert.equal(personTier([{ cents: null, at: ago(1) }], NOW).tier, null);
});

test("person links must be allowlisted profiles; corporate links any https site", () => {
  assert.equal(checkLinkUrl("https://github.com/someone").ok, true);
  assert.equal(checkLinkUrl("https://www.artstation.com/someone").host, "artstation.com");
  assert.equal(checkLinkUrl("https://github.com/").ok, false);
  assert.equal(checkLinkUrl("http://github.com/someone").ok, false);
  assert.equal(checkLinkUrl("https://evil.example/someone").ok, false);
  assert.equal(checkLinkUrl("https://user:pw@github.com/x").ok, false);
  assert.equal(checkLinkUrl("https://github.com:8443/x").ok, false);
  assert.equal(checkLinkUrl("https://github.com.evil.example/x").ok, false);
  assert.equal(checkLinkUrl("https://studio.example", { corporate: true }).ok, true);
  assert.equal(checkLinkUrl("not a url").ok, false);
});

test("a redirect off the approved host pulls the link", () => {
  assert.equal(judgeLink("github.com", [{ url: "https://github.com/a", status: 200 }]).verdict, "ok");
  assert.equal(judgeLink("github.com", [{ url: "https://github.com/a", status: 301 }, { url: "https://www.github.com/a/", status: 200 }]).verdict, "ok");
  const off = judgeLink("github.com", [{ url: "https://github.com/a", status: 302 }, { url: "https://bad.example/", status: 200 }]);
  assert.equal(off.verdict, "pulled");
  assert.match(off.reason, /bad\.example/);
  assert.equal(judgeLink("github.com", [{ url: "https://github.com/a", status: 404 }]).verdict, "dead");
  assert.equal(judgeLink("github.com", [{ url: "https://github.com/a", error: "timeout" }]).verdict, "dead");
  assert.equal(judgeLink("github.com", [{ url: "https://github.com/a", status: 403 }]).verdict, "inconclusive");
  assert.equal(judgeLink("github.com", [{ url: "https://github.com/a", status: 200 }], true).verdict, "pulled");
});

test("a dead link drops after 30 days and recovers before then", () => {
  const first = applyVerdict({ status: "ok" }, { verdict: "dead", reason: "HTTP 404" }, NOW);
  assert.equal(first.status, undefined);
  assert.equal(first.dead_since, new Date(NOW).toISOString());
  const later = applyVerdict({ status: "ok", dead_since: ago(31) }, { verdict: "dead" }, NOW);
  assert.equal(later.status, "dead");
  assert.equal(applyVerdict({ status: "ok", dead_since: ago(5) }, { verdict: "ok" }, NOW).dead_since, null);
});

test("one link change per 12 months, one restore, never past a ban", () => {
  assert.equal(canSetLink(null, "https://github.com/a", NOW).ok, true);
  const recent = { url: "https://github.com/a", changed_at: ago(100), status: "ok", strikes: 0 };
  assert.equal(canSetLink(recent, "https://github.com/b", NOW).ok, false);
  assert.equal(canSetLink(recent, "https://github.com/a", NOW).ok, true);
  assert.equal(canSetLink({ ...recent, changed_at: ago(400) }, "https://github.com/b", NOW).ok, true);
  assert.equal(canSetLink({ ...recent, status: "pulled", strikes: 1 }, "https://github.com/a", NOW).ok, true);
  assert.equal(canSetLink({ ...recent, status: "pulled", strikes: 2 }, "https://github.com/a", NOW).ok, false);
  assert.equal(canSetLink({ ...recent, banned: 1 }, "https://github.com/a", NOW, true).ok, false);
});

test("credits list opted-in named tiers only, highest first, links for Eternal only", () => {
  const people = [
    { id: "a", name: "Ann", public: true, tier: "supporter" },
    { id: "b", name: "Bo", public: true, tier: "eternal" },
    { id: "c", name: "Cy", public: false, tier: "patron" },
    { id: "d", name: "Di", public: true, tier: "backer" },
    { id: "e", name: "Ed", public: true, tier: "legendary", number: 2 },
    { id: "f", name: "Rude", public: true, tier: "patron" },
    { id: "g", name: "Gil", public: true, tier: "patron" },
  ];
  const links = {
    "person:b": { url: "https://github.com/bo", status: "ok" },
    "person:g": { url: "https://github.com/gil", status: "ok" },
    "contributor:Kai": { url: "https://github.com/kai", status: "pulled" },
  };
  const settings = {
    hide: ["rude"],
    contributors: [{ name: "Kai", role: "Engineering review and testing", areas: ["OpenUSD", "Hydra"] }],
    testers: [{ name: "Zed" }, { name: "Lu", founding: true }],
  };
  const c = buildCredits({ people, links, settings, now: NOW });
  assert.deepEqual(c.supporters.map((s) => s.name), ["Bo", "Ed", "Gil", "Ann"]);
  assert.equal(c.supporters[0].url, "https://github.com/bo");
  assert.equal(c.supporters[2].url, undefined);
  assert.equal(c.supporters[1].number, 2);
  assert.equal(c.contributors[0].url, undefined);
  assert.deepEqual(c.testers.map((t) => t.name), ["Lu", "Zed"]);
});

test("Ko-fi webhook fields; unknown currencies have no amount", () => {
  const k = parseKofi({ kofi_transaction_id: "t1", amount: "5.00", currency: "USD", email: " A@B.C ", from_name: "Ann", is_public: true, timestamp: "2026-11-01T00:00:00Z" });
  assert.equal(k.cents, 500);
  assert.equal(k.email, "a@b.c");
  assert.equal(parseKofi({ kofi_transaction_id: "t2", amount: "5", currency: "NZD" }).cents, null);
  assert.equal(parseKofi({ kofi_transaction_id: "t3", amount: "10", currency: "NZD" }, { NZD: 0.6 }).cents, 600);
  assert.equal(parseKofi({}), null);
});

function env() {
  return { CREDITS_DB: fakeD1(), ADMIN_TOKEN: "secret", KOFI_TOKEN: "kt" };
}

const kofiPost = (data) => {
  const form = new FormData();
  form.set("data", JSON.stringify(data));
  return new Request("https://w/credits/kofi", { method: "POST", body: form });
};

const adminPost = (path, body, token = "secret") =>
  new Request(`https://w${path}`, { method: "POST", headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json" }, body: JSON.stringify(body) });

test("Ko-fi donations add up to a tier and publish", async () => {
  const e = env();
  const base = { verification_token: "kt", currency: "USD", email: "ann@x.y", from_name: "Ann", is_public: true, timestamp: ago(1) };
  assert.equal((await handleCredits(kofiPost({ ...base, kofi_transaction_id: "1", amount: "20" }), e, NOW)).status, 200);
  assert.equal((await handleCredits(kofiPost({ ...base, kofi_transaction_id: "2", amount: "15" }), e, NOW)).status, 200);
  // A replayed webhook doesn't count twice.
  await handleCredits(kofiPost({ ...base, kofi_transaction_id: "2", amount: "15" }), e, NOW);
  const res = await handleCredits(new Request("https://w/credits.json"), e, NOW);
  const credits = await res.json();
  assert.deepEqual(credits.supporters, [{ name: "Ann", tier: "supporter" }]);
  assert.equal(res.headers.get("Access-Control-Allow-Origin"), "*");

  assert.equal((await handleCredits(kofiPost({ ...base, verification_token: "no", kofi_transaction_id: "3", amount: "1" }), e, NOW)).status, 403);
});

test("refunds drop the tier; legendaries keep their number", async () => {
  const e = env();
  await handleCredits(adminPost("/admin/payment", { id: "m1", donor_id: "manual:x", name: "Xi", public: true, cents: 60000 }), e, NOW);
  await handleCredits(adminPost("/admin/payment", { id: "m2", donor_id: "manual:y", name: "Yo", public: true, cents: 60000 }), e, NOW);
  let credits = await publish(e, NOW);
  assert.deepEqual(credits.supporters.map((s) => [s.name, s.number]), [["Xi", 1], ["Yo", 2]]);
  await handleCredits(adminPost("/admin/payment", { id: "m1", reversed: true }), e, NOW);
  credits = await publish(e, NOW);
  assert.deepEqual(credits.supporters.map((s) => s.name), ["Yo"]);
});

test("admin routes need the token", async () => {
  const e = env();
  assert.equal((await handleCredits(adminPost("/admin/publish", {}, "wrong"), e, NOW)).status, 401);
  assert.equal((await handleCredits(adminPost("/admin/publish", {}), e, NOW)).status, 200);
});

test("Eternal links are published, checked nightly and pulled when they redirect away", async () => {
  const e = env();
  await handleCredits(adminPost("/admin/payment", { id: "big", donor_id: "manual:e", name: "Eve", public: true, cents: 5000000, at: ago(200) }), e, NOW);
  await recompute(e, NOW);
  const set = await handleCredits(adminPost("/admin/link", { owner: "person:manual:e", url: "https://github.com/eve" }), e, NOW);
  assert.equal(set.status, 200);
  assert.equal((await publish(e, NOW)).supporters[0].url, "https://github.com/eve");

  const bad = await handleCredits(adminPost("/admin/link", { owner: "person:manual:e", url: "https://evil.example/eve" }), e, NOW);
  assert.equal(bad.status, 400);

  await checkLinks(e, NOW, async (url) => [{ url, status: 302 }, { url: "https://evil.example/", status: 200 }]);
  const credits = await publish(e, NOW);
  assert.equal(credits.supporters[0].url, undefined);
  assert.equal(credits.supporters[0].name, "Eve");
  const alerts = await e.CREDITS_DB.prepare("SELECT message FROM alerts").bind().all();
  assert.match(alerts.results[0].message, /evil\.example/);

  // Restored once after a compromise; a second pull needs force to come back.
  assert.equal((await setLink(e, "person:manual:e", "https://github.com/eve", { now: NOW })).ok, true);
  await handleCredits(adminPost("/admin/pull", { owner: "person:manual:e" }), e, NOW);
  assert.equal((await setLink(e, "person:manual:e", "https://github.com/eve", { now: NOW })).ok, false);
  await handleCredits(adminPost("/admin/pull", { owner: "person:manual:e", ban: true }), e, NOW);
  assert.equal((await setLink(e, "person:manual:e", "https://github.com/eve", { now: NOW, force: true })).ok, false);
});

test("reports are stored and alerted", async () => {
  const e = env();
  const res = await handleCredits(new Request("https://w/credits/report", { method: "POST", body: JSON.stringify({ name: "Eve", url: "https://github.com/eve", reason: "spam" }) }), e, NOW);
  assert.equal(res.status, 200);
  const rows = await e.CREDITS_DB.prepare("SELECT * FROM reports").bind().all();
  assert.equal(rows.results.length, 1);
});

test("without the D1 binding the credits routes answer 503 and other paths fall through", async () => {
  assert.equal((await handleCredits(new Request("https://w/credits.json"), {}, NOW)).status, 503);
  assert.equal(await handleCredits(new Request("https://w/other"), {}, NOW), null);
});
