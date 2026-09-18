/**
 * worker.js: tiny service that collects firmware version reports from devices.
 *
 * Added 2026-09-18 for v1.2 (docs/CLOUD_UPDATES.md). Runs on Cloudflare Workers'
 * free tier. Devices can't be reached from the internet (home routers block
 * inbound connections), so instead each device POSTs what it is running after
 * every boot and update check. This service stores the last report per device
 * and shows them, which is how you confirm remotely that an update landed.
 *
 * Endpoints:
 *   POST /report   device -> here. Body: {"device":"e01c64","version":"1.2.0",
 *                  "slot":"ota_1","status":"running","uptime_s":42}
 *   GET  /devices  JSON list of every device and its last report (for scripts)
 *   GET  /         human-readable HTML table of the same data
 *
 * Storage: a Workers KV namespace bound as DEVICES. One key per device, holding
 * the last report plus the time it arrived. Reports are small, so the free tier
 * is ample.
 *
 * Deliberately NOT authenticated: the data is only "which firmware is where",
 * and devices would need a secret embedded in firmware to sign reports, which
 * is awkward to rotate. If that changes, add a shared token here and in
 * CONFIG_APP_CLOUD_REPORT_URL, or sign reports with a per-device key.
 */

/** How long a device may be silent before the page marks it stale (minutes). */
const STALE_AFTER_MINUTES = 90;

export default {
  /**
   * Cloudflare entry point: routes one request.
   * @param {Request} request
   * @param {{DEVICES: KVNamespace}} env KV binding declared in wrangler.toml
   */
  async fetch(request, env) {
    const url = new URL(request.url);

    if (request.method === "POST" && url.pathname === "/report") {
      return handleReport(request, env);
    }
    if (request.method === "GET" && url.pathname === "/devices") {
      return handleList(env, "json");
    }
    if (request.method === "GET" && url.pathname === "/") {
      return handleList(env, "html");
    }
    return new Response("Not found\n", { status: 404 });
  },
};

/**
 * Store one device report. Validates just enough to keep junk out of storage.
 */
async function handleReport(request, env) {
  let body;
  try {
    body = await request.json();
  } catch {
    return new Response("Body must be JSON\n", { status: 400 });
  }

  // Device id is the MAC suffix the firmware sends, e.g. "e01c64". Restricting
  // the shape stops a stray request from creating odd keys in storage.
  const device = String(body.device || "").toLowerCase();
  if (!/^[0-9a-f]{6}$/.test(device)) {
    return new Response('Expected "device" as 6 hex characters\n', { status: 400 });
  }

  const record = {
    device,
    version: String(body.version || "unknown").slice(0, 32),
    slot: String(body.slot || "?").slice(0, 16),
    status: String(body.status || "running").slice(0, 32),
    uptime_s: Number(body.uptime_s) || 0,
    reported_at: new Date().toISOString(),
  };

  await env.DEVICES.put(device, JSON.stringify(record));
  return Response.json({ ok: true, stored: record });
}

/**
 * Return every device's last report, as JSON or as a small HTML table.
 */
async function handleList(env, format) {
  const listing = await env.DEVICES.list();
  const records = [];
  for (const key of listing.keys) {
    const raw = await env.DEVICES.get(key.name);
    if (raw) {
      records.push(JSON.parse(raw));
    }
  }
  // Most recently heard from first.
  records.sort((a, b) => (a.reported_at < b.reported_at ? 1 : -1));

  if (format === "json") {
    return Response.json({ devices: records });
  }

  const rows = records
    .map((r) => {
      const ageMin = (Date.now() - Date.parse(r.reported_at)) / 60000;
      const stale = ageMin > STALE_AFTER_MINUTES;
      return `<tr class="${stale ? "stale" : ""}">
        <td><code>${r.device}</code></td>
        <td><b>${r.version}</b></td>
        <td>${r.slot}</td>
        <td>${r.status}</td>
        <td>${formatAge(ageMin)}${stale ? " (stale)" : ""}</td>
      </tr>`;
    })
    .join("");

  const html = `<!doctype html><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>Device firmware versions</title>
<style>
 body{font-family:system-ui,sans-serif;margin:2rem;max-width:48rem}
 table{border-collapse:collapse;width:100%}
 th,td{text-align:left;padding:.5rem .75rem;border-bottom:1px solid #ddd}
 tr.stale{color:#999}
 caption{text-align:left;padding-bottom:.75rem;color:#666}
</style>
<h1>Device firmware versions</h1>
<table>
 <caption>Each device reports after every boot and update check. Devices quiet for
 more than ${STALE_AFTER_MINUTES} minutes are greyed out.</caption>
 <tr><th>Device</th><th>Version</th><th>Slot</th><th>Status</th><th>Last report</th></tr>
 ${rows || '<tr><td colspan="5">No reports yet.</td></tr>'}
</table>`;

  return new Response(html, { headers: { "content-type": "text/html; charset=utf-8" } });
}

/** Human-friendly age, e.g. "3 min ago" or "2 h ago". */
function formatAge(minutes) {
  if (minutes < 1) return "just now";
  if (minutes < 60) return `${Math.round(minutes)} min ago`;
  const hours = minutes / 60;
  if (hours < 48) return `${Math.round(hours)} h ago`;
  return `${Math.round(hours / 24)} days ago`;
}
