const PROXY_URL = '/.netlify/functions/proxy';
const STORAGE_URL = 'https://xdxlfkyywnjrzqblvdzg.supabase.co/storage/v1/object/public/Netpen/version.txt';

export async function proxyFetch(path, options = {}) {
  const url = `${PROXY_URL}?path=${encodeURIComponent(path)}`;
  const res = await fetch(url, {
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

export function proxyPost(path, body, method = 'POST', headers = {}) {
  return proxyFetch(path, { method, headers, body: JSON.stringify(body) });
}

export async function getLatestVersion() {
  const res = await fetch(STORAGE_URL);
  if (!res.ok) throw new Error(`${res.status} ${res.statusText}`);
  return parseInt(await res.text(), 10);
}

export async function discordRequest(token, path, options = {}) {
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
