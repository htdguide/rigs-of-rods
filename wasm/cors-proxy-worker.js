/**
 * Minimal CORS proxy for the Rigs of Rods web build (Cloudflare Worker).
 *
 * Why this exists: forum.rigsofrods.org serves repository downloads correctly
 * (HTTP 200, correct bytes) but sends no Access-Control-Allow-Origin header, so
 * a browser fetch from the wasm build is blocked. This re-fetches the file
 * server-side and returns it with the header attached.
 *
 * Deploy:
 *   npm i -g wrangler
 *   wrangler init ror-cors-proxy   # or drop this in an existing worker
 *   wrangler deploy
 *
 * Then open the game with the proxy in the URL:
 *   https://<your-pages-site>/?cors_proxy=https://ror-cors-proxy.<you>.workers.dev/?url=
 *
 * The trailing "?url=" matters: WasmProxiedUrl percent-encodes the target when
 * the proxy string ends with '=', which a repository download URL needs because
 * it carries its own "?file=NNNN" query.
 *
 * NOTE: this is deliberately NOT an open relay. Only the hosts below are
 * proxied, so it cannot be used to launder arbitrary traffic through your
 * Cloudflare account.
 */

const ALLOWED_HOSTS = [
  'forum.rigsofrods.org',
  'v2.api.rigsofrods.org',
];

// Cap the proxied body so a runaway request cannot burn the free-tier quota.
const MAX_BYTES = 200 * 1024 * 1024;

function corsHeaders(extra = {}) {
  return {
    'Access-Control-Allow-Origin': '*',
    'Access-Control-Allow-Methods': 'GET, HEAD, OPTIONS',
    'Access-Control-Allow-Headers': '*',
    'Access-Control-Expose-Headers': 'Content-Length, Content-Type, Content-Disposition',
    ...extra,
  };
}

export default {
  async fetch(request) {
    if (request.method === 'OPTIONS') {
      return new Response(null, { status: 204, headers: corsHeaders() });
    }
    if (request.method !== 'GET' && request.method !== 'HEAD') {
      return new Response('Only GET/HEAD are proxied.', { status: 405, headers: corsHeaders() });
    }

    // Target arrives either as ?url=<encoded> or as the rest of the path.
    const here = new URL(request.url);
    let target = here.searchParams.get('url');
    if (!target) {
      // Tolerate the prefix style: https://worker/https://forum.../download?file=1
      const raw = here.href.slice(here.origin.length + 1);
      target = raw.startsWith('http') ? raw : '';
    }
    if (!target) {
      return new Response('Pass the target as ?url=<encoded URL>.', { status: 400, headers: corsHeaders() });
    }

    let dest;
    try {
      dest = new URL(target);
    } catch {
      return new Response('Malformed target URL.', { status: 400, headers: corsHeaders() });
    }
    if (dest.protocol !== 'https:' || !ALLOWED_HOSTS.includes(dest.hostname)) {
      return new Response(
        `Refusing to proxy ${dest.hostname}. Allowed: ${ALLOWED_HOSTS.join(', ')}`,
        { status: 403, headers: corsHeaders() });
    }

    const upstream = await fetch(dest.toString(), {
      method: request.method,
      headers: { 'User-Agent': 'Rigs of Rods Client (web)', 'Accept': '*/*' },
      redirect: 'follow',
    });

    const len = Number(upstream.headers.get('Content-Length') || 0);
    if (len > MAX_BYTES) {
      return new Response(`Upstream body too large (${len} bytes).`, { status: 413, headers: corsHeaders() });
    }

    // Stream the body straight through; only the headers are rewritten.
    const headers = corsHeaders({
      'Content-Type': upstream.headers.get('Content-Type') || 'application/octet-stream',
      'Cache-Control': 'public, max-age=3600',
    });
    const cl = upstream.headers.get('Content-Length');
    if (cl) headers['Content-Length'] = cl;
    const cd = upstream.headers.get('Content-Disposition');
    if (cd) headers['Content-Disposition'] = cd;

    return new Response(upstream.body, { status: upstream.status, headers });
  },
};
