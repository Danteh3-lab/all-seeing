const SUPABASE_URL = 'https://xdxlfkyywnjrzqblvdzg.supabase.co';
const ANON_KEY = 'eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InhkeGxma3l5d25qcnpxYmx2ZHpnIiwicm9sZSI6ImFub24iLCJpYXQiOjE3Nzk4MTcwOTgsImV4cCI6MjA5NTM5MzA5OH0.ouEQ_0iNr7xT7FpaySMvNlGTg37z_rVKPD7CqKiALpo';

const handler = async (event) => {
  const path = event.queryStringParameters.path || '';
  const url = SUPABASE_URL + path;

  try {
    const headers = {
      'apikey': ANON_KEY,
      'Authorization': 'Bearer ' + ANON_KEY,
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
