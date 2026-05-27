const SUPABASE_URL = process.env.SUPABASE_URL || 'https://xdxlfkyywnjrzqblvdzg.supabase.co';

const handler = async (event) => {
  const path = event.queryStringParameters.path || '';
  if (!path.startsWith('/rest/v1/')) {
    return { statusCode: 400, body: JSON.stringify({ error: 'Invalid path' }) };
  }
  const url = SUPABASE_URL + path;

  try {
    const headers = {
      'apikey': process.env.SUPABASE_ANON_KEY,
      'Authorization': 'Bearer ' + process.env.SUPABASE_ANON_KEY,
      'Content-Type': 'application/json'
    };

    const res = await fetch(url, {
      method: event.httpMethod,
      headers,
      body: event.body || null
    });

    const body = await res.text();
    return {
      statusCode: res.status,
      headers: { 'Content-Type': 'application/json' },
      body
    };
  } catch (err) {
    return {
      statusCode: 500,
      body: JSON.stringify({ error: err.message })
    };
  }
};

export { handler };
