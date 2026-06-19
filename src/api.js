const PROXY_URL = '/.netlify/functions/proxy';
const VERSION_URL = 'https://xdxlfkyywnjrzqblvdzg.supabase.co/storage/v1/object/public/Netpen/version.txt';

export async function proxyFetch(path, options = {}) {
  const res = await fetch(`${PROXY_URL}?path=${encodeURIComponent(path)}`, {
    ...options,
    headers: {
      'Content-Type': 'application/json',
      ...(options.headers || {})
    }
  });
  if (!res.ok) throw new Error(`${res.status} ${res.statusText}`);
  const text = await res.text();
  return text ? JSON.parse(text) : null;
}

export function proxySend(path, payload, method = 'POST', headers = {}) {
  return proxyFetch(path, {
    method,
    headers,
    body: JSON.stringify(payload)
  });
}

export async function fetchLatestVersion() {
  const res = await fetch(VERSION_URL);
  if (!res.ok) throw new Error(`${res.status} ${res.statusText}`);
  return parseInt(await res.text(), 10);
}

export async function discordApi(token, path, options = {}) {
  const res = await fetch(`https://discord.com/api/v9${path}`, {
    ...options,
    headers: {
      Authorization: token,
      'Content-Type': 'application/json',
      ...(options.headers || {})
    }
  });
  if (!res.ok) {
    const text = await res.text();
    throw new Error(text || `${res.status} ${res.statusText}`);
  }
  const text = await res.text();
  return text ? JSON.parse(text) : null;
}
