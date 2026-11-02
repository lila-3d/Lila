// Credits rules with no I/O: tiers, link validation, link check verdicts and the published credits.json.
// The rules come from the Supporter Terms; keep the two in step.

export const TIERS = [
  { id: "backer", cents: 1000 },
  { id: "supporter", cents: 3000 },
  { id: "patron", cents: 10000 },
  { id: "legendary", cents: 50000 },
  { id: "mythic", cents: 500000 },
  { id: "eternal", cents: 5000000 },
];

const RANK = Object.fromEntries(TIERS.map((t, i) => [t.id, i]));

/** Tiers whose names are listed; Backer is a badge only. */
export const NAMED_TIERS = ["eternal", "mythic", "legendary", "patron", "supporter"];

export const DEFAULT_ALLOWLIST = [
  "github.com",
  "artstation.com",
  "linkedin.com",
  "youtube.com",
  "x.com",
];

export const DAY_MS = 24 * 60 * 60 * 1000;
export const LINK_CHANGE_DAYS = 365;
export const DEAD_DAYS = 30;
export const CHARGEBACK_DAYS = 120;

export function tierRank(id) {
  return id in RANK ? RANK[id] : -1;
}

export function tierFor(cents) {
  let tier = null;
  for (const t of TIERS) if (cents >= t.cents) tier = t.id;
  return tier;
}

/**
 * A person's tier from their payments ({cents, at, reversed}). Mythic and up count only payments older than the
 * chargeback window, so a reversed large payment can't leave a permanent named line behind.
 */
export function personTier(payments, now, chargebackDays = CHARGEBACK_DAYS) {
  let total = 0;
  let cleared = 0;
  for (const p of payments) {
    if (p.reversed || !Number.isFinite(p.cents)) continue;
    total += p.cents;
    if (now - Date.parse(p.at) >= chargebackDays * DAY_MS) cleared += p.cents;
  }
  let tier = tierFor(total);
  if (tierRank(tier) >= RANK.mythic) {
    const clearedTier = tierFor(cleared);
    tier = tierRank(clearedTier) >= RANK.mythic ? clearedTier : "legendary";
  }
  return { tier, total, cleared };
}

/** Lower-case host without one leading "www.", so www.github.com and github.com compare equal. */
export function bareHost(host) {
  const h = String(host || "").toLowerCase();
  return h.startsWith("www.") ? h.slice(4) : h;
}

/**
 * Whether url may be submitted as a link. Person and contributor links must be a profile (not the site root) on an
 * allowlisted platform; corporate links may be any https site.
 */
export function checkLinkUrl(url, { corporate = false, allowlist = DEFAULT_ALLOWLIST } = {}) {
  let u;
  try {
    u = new URL(url);
  } catch {
    return { ok: false, reason: "not a URL" };
  }
  if (u.protocol !== "https:") return { ok: false, reason: "must be https" };
  if (u.username || u.password) return { ok: false, reason: "must not carry a user name or password" };
  if (u.port) return { ok: false, reason: "must not name a port" };
  const host = bareHost(u.hostname);
  if (!corporate) {
    if (!allowlist.map(bareHost).includes(host)) return { ok: false, reason: `${host} is not an allowed platform` };
    if (u.pathname.replace(/\/+$/, "") === "") return { ok: false, reason: "must be a profile, not the site's front page" };
  }
  return { ok: true, host, url: u.toString() };
}

/**
 * The verdict on one nightly check. hops is the redirect chain as fetched, [{url, status}] ending with the final
 * response, or [{url, error}] when the request failed.
 *   pulled: it now leads somewhere other than the approved host, or a scanner flagged it.
 *   dead: gone (404/410) or unreachable; dropped once dead for DEAD_DAYS.
 *   inconclusive: blocked or rate limited (401/403/429/5xx); changes nothing.
 */
export function judgeLink(approvedHost, hops, flagged = false) {
  if (flagged) return { verdict: "pulled", reason: "flagged by a security scanner" };
  const approved = bareHost(approvedHost);
  for (const hop of hops) {
    let host;
    try {
      host = bareHost(new URL(hop.url).hostname);
    } catch {
      return { verdict: "pulled", reason: "redirects to an invalid URL" };
    }
    if (host !== approved) return { verdict: "pulled", reason: `redirects to ${host}` };
  }
  const last = hops[hops.length - 1];
  if (!last || last.error) return { verdict: "dead", reason: last?.error || "no response" };
  if (last.status >= 200 && last.status < 300) return { verdict: "ok" };
  if (last.status === 404 || last.status === 410) return { verdict: "dead", reason: `HTTP ${last.status}` };
  if (last.status >= 300 && last.status < 400) return { verdict: "pulled", reason: "too many redirects" };
  return { verdict: "inconclusive", reason: `HTTP ${last.status}` };
}

/** The link's state after a verdict; link is a links row. Returns the fields to update. */
export function applyVerdict(link, result, now) {
  const at = new Date(now).toISOString();
  if (result.verdict === "ok") return { status: "ok", dead_since: null, checked_at: at, reason: null };
  if (result.verdict === "pulled")
    return { status: "pulled", checked_at: at, reason: result.reason, strikes: (link.strikes || 0) + 1 };
  if (result.verdict === "dead") {
    const since = link.dead_since || at;
    if (now - Date.parse(since) >= DEAD_DAYS * DAY_MS)
      return { status: "dead", dead_since: since, checked_at: at, reason: `dead since ${since.slice(0, 10)}` };
    return { dead_since: since, checked_at: at, reason: result.reason };
  }
  return { checked_at: at };
}

/** Whether a link row is shown in the published credits. */
export function linkLive(link) {
  return !!link && !link.banned && (link.status === "approved" || link.status === "ok");
}

/**
 * Whether a new link for owner may be set now. existing is its links row or null. force skips the change limit and the
 * one-restore rule, never a ban.
 */
export function canSetLink(existing, url, now, force = false) {
  if (!existing) return { ok: true };
  if (existing.banned) return { ok: false, reason: "links are banned for this owner" };
  if (force) return { ok: true };
  if (existing.status === "pulled" && (existing.strikes || 0) >= 2)
    return { ok: false, reason: "pulled twice; a link can be restored only once" };
  const sameUrl = existing.url === url;
  if (!sameUrl && existing.changed_at && now - Date.parse(existing.changed_at) < LINK_CHANGE_DAYS * DAY_MS)
    return { ok: false, reason: `changed on ${existing.changed_at.slice(0, 10)}; one change per 12 months` };
  return { ok: true };
}

function hidden(name, hideList) {
  const n = String(name || "").trim().toLowerCase();
  return !n || hideList.some((h) => String(h).trim().toLowerCase() === n);
}

/**
 * The published credits.json.
 *   people: [{id, name, public, hidden, tier, number, founder}] with tiers already worked out.
 *   links: {owner: links row}, owner "person:<id>", "contributor:<name>" or "corporate:<name>".
 *   settings: {corporate, pastSponsors, contributors, testers, hide}.
 * Person links are published for Eternal only.
 */
export function buildCredits({ people, links, settings, now }) {
  const hide = settings.hide || [];
  const linkFor = (owner) => (linkLive(links[owner]) ? links[owner].url : undefined);
  const withUrl = (entry, owner) => {
    const url = linkFor(owner);
    return url ? { ...entry, url } : entry;
  };

  const supporters = people
    .filter((p) => p.public && !p.hidden && NAMED_TIERS.includes(p.tier) && !hidden(p.name, hide))
    .map((p) => {
      let entry = { name: p.name.trim(), tier: p.tier };
      if (p.number) entry.number = p.number;
      if (p.founder) entry.founder = true;
      if (p.tier === "eternal") entry = withUrl(entry, `person:${p.id}`);
      return entry;
    })
    .sort((a, b) => tierRank(b.tier) - tierRank(a.tier) || (a.number || 0) - (b.number || 0) || a.name.localeCompare(b.name));

  const corporate = (settings.corporate || []).map((c) => withUrl({ name: c.name, logo: c.logo, since: c.since }, `corporate:${c.name}`));
  const pastSponsors = (settings.pastSponsors || []).map((c) => ({ name: c.name, logo: c.logo }));
  const contributors = (settings.contributors || []).map((c) =>
    withUrl({ name: c.name, role: c.role, areas: c.areas || [], since: c.since }, `contributor:${c.name}`));
  const testers = (settings.testers || [])
    .map((t) => ({ name: t.name, founding: !!t.founding }))
    .sort((a, b) => Number(b.founding) - Number(a.founding) || a.name.localeCompare(b.name));

  const strip = (o) => Object.fromEntries(Object.entries(o).filter(([, v]) => v !== undefined));
  return {
    version: 1,
    updated: new Date(now).toISOString(),
    corporate: corporate.map(strip),
    pastSponsors: pastSponsors.map(strip),
    supporters,
    contributors: contributors.map(strip),
    testers,
  };
}

/** The fields Lila needs from a Ko-fi webhook body (the form field "data", already parsed). */
export function parseKofi(data, fx = { USD: 1 }) {
  if (!data || typeof data !== "object" || !data.kofi_transaction_id) return null;
  const amount = Number.parseFloat(data.amount);
  const rate = fx[String(data.currency || "USD").toUpperCase()];
  return {
    transactionId: String(data.kofi_transaction_id),
    email: String(data.email || "").trim().toLowerCase(),
    name: String(data.from_name || "").trim().slice(0, 64),
    public: data.is_public === true || data.is_public === "true",
    cents: Number.isFinite(amount) && rate ? Math.round(amount * rate * 100) : null,
    currency: String(data.currency || ""),
    at: data.timestamp ? new Date(data.timestamp).toISOString() : new Date().toISOString(),
  };
}
