// Chezz billing webhook.
//
// Stripe tells this worker when someone subscribes, changes plan or cancels; the worker writes the
// member's plan into Firestore, where the app reads it. It runs on Cloudflare Workers' free tier.
// The app never holds a Stripe key: it only opens a payment page in the member's browser with
// their membership id attached as client_reference_id.
//
// Secrets it needs (see server/README.md):
//   STRIPE_WEBHOOK_SECRET   whsec_...
//   GOOGLE_SERVICE_ACCOUNT  the whole service-account JSON, as one line
//   FIREBASE_PROJECT_ID     e.g. chezz-game3
//   PRICE_SILVER / PRICE_GOLD / PRICE_RUBY   the Stripe price ids for each plan

const TEXT = new TextEncoder();

function b64url(bytes) {
  let s = '';
  for (const b of bytes) s += String.fromCharCode(b);
  return btoa(s).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
}

function hex(buffer) {
  return [...new Uint8Array(buffer)].map((b) => b.toString(16).padStart(2, '0')).join('');
}

// Constant-time-ish compare, so a wrong signature tells an attacker nothing about how wrong it was.
function sameString(a, b) {
  if (a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return diff === 0;
}

// ---------------------------------------------------------------- Stripe signature

async function verifyStripe(raw, header, secret) {
  if (!header) return false;
  let timestamp = '';
  const signatures = [];
  for (const part of header.split(',')) {
    const i = part.indexOf('=');
    const key = part.slice(0, i).trim();
    const value = part.slice(i + 1).trim();
    if (key === 't') timestamp = value;
    else if (key === 'v1') signatures.push(value);
  }
  if (!timestamp || !signatures.length) return false;
  // reject anything older than five minutes, so a captured request cannot be replayed later
  if (Math.abs(Math.floor(Date.now() / 1000) - Number(timestamp)) > 300) return false;
  const key = await crypto.subtle.importKey('raw', TEXT.encode(secret), { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
  const mac = hex(await crypto.subtle.sign('HMAC', key, TEXT.encode(`${timestamp}.${raw}`)));
  return signatures.some((s) => sameString(s, mac));
}

// ---------------------------------------------------------------- Firestore, as the service account

let cachedToken = null;

async function accessToken(env) {
  if (cachedToken && cachedToken.expires > Date.now() + 60000) return cachedToken.value;
  const sa = JSON.parse(env.GOOGLE_SERVICE_ACCOUNT);
  const pem = sa.private_key.replace(/-----[A-Z ]+-----/g, '').replace(/\s+/g, '');
  const der = Uint8Array.from(atob(pem), (c) => c.charCodeAt(0));
  const key = await crypto.subtle.importKey('pkcs8', der, { name: 'RSASSA-PKCS1-v1_5', hash: 'SHA-256' }, false, ['sign']);
  const now = Math.floor(Date.now() / 1000);
  const body = `${b64url(TEXT.encode(JSON.stringify({ alg: 'RS256', typ: 'JWT' })))}.${b64url(
    TEXT.encode(
      JSON.stringify({
        iss: sa.client_email,
        scope: 'https://www.googleapis.com/auth/datastore',
        aud: 'https://oauth2.googleapis.com/token',
        iat: now,
        exp: now + 3600,
      })
    )
  )}`;
  const signature = b64url(new Uint8Array(await crypto.subtle.sign('RSASSA-PKCS1-v1_5', key, TEXT.encode(body))));
  const reply = await fetch('https://oauth2.googleapis.com/token', {
    method: 'POST',
    headers: { 'content-type': 'application/x-www-form-urlencoded' },
    body: `grant_type=${encodeURIComponent('urn:ietf:params:oauth:grant-type:jwt-bearer')}&assertion=${body}.${signature}`,
  });
  const json = await reply.json();
  if (!json.access_token) throw new Error(`google token: ${JSON.stringify(json)}`);
  cachedToken = { value: json.access_token, expires: Date.now() + (json.expires_in - 120) * 1000 };
  return cachedToken.value;
}

function docUrl(env, path) {
  return `https://firestore.googleapis.com/v1/projects/${env.FIREBASE_PROJECT_ID}/databases/(default)/documents/${path}`;
}

async function readDoc(env, path) {
  const reply = await fetch(docUrl(env, path), { headers: { authorization: `Bearer ${await accessToken(env)}` } });
  if (reply.status === 404) return null;
  if (!reply.ok) throw new Error(`firestore read ${path}: ${reply.status}`);
  return (await reply.json()).fields || {};
}

async function writeDoc(env, path, fields) {
  const mask = Object.keys(fields).map((k) => `updateMask.fieldPaths=${k}`).join('&');
  const reply = await fetch(`${docUrl(env, path)}?${mask}`, {
    method: 'PATCH',
    headers: { authorization: `Bearer ${await accessToken(env)}`, 'content-type': 'application/json' },
    body: JSON.stringify({ fields }),
  });
  if (!reply.ok) throw new Error(`firestore write ${path}: ${reply.status} ${await reply.text()}`);
}

const str = (v) => ({ stringValue: String(v) });
const num = (v) => ({ integerValue: String(v) });

// ---------------------------------------------------------------- the events that matter

function planForPrice(env, priceId) {
  if (priceId && priceId === env.PRICE_RUBY) return 'ruby';
  if (priceId && priceId === env.PRICE_GOLD) return 'gold';
  if (priceId && priceId === env.PRICE_SILVER) return 'silver';
  return null;
}

async function uidForCustomer(env, customer) {
  if (!customer) return null;
  const doc = await readDoc(env, `billingCustomers/${customer}`);
  return doc && doc.uid ? doc.uid.stringValue : null;
}

async function setPlan(env, uid, plan, customer) {
  const fields = { plan: str(plan), updated: num(Date.now()) };
  if (customer) fields.customer = str(customer);
  await writeDoc(env, `plans/${uid}`, fields);
}

async function handle(event, env) {
  const object = event.data.object;

  if (event.type === 'checkout.session.completed') {
    // the only event that carries the membership id, so this is where customer -> uid is recorded
    const uid = object.client_reference_id;
    const customer = object.customer;
    if (!uid || !customer) return 'ignored: no membership id on the session';
    await writeDoc(env, `billingCustomers/${customer}`, { uid: str(uid), updated: num(Date.now()) });
    await writeDoc(env, `plans/${uid}`, { customer: str(customer), updated: num(Date.now()) });
    return `linked ${customer} to ${uid}`;
  }

  if (event.type === 'customer.subscription.created' || event.type === 'customer.subscription.updated') {
    const uid = await uidForCustomer(env, object.customer);
    // the two events can arrive in either order: ask Stripe to deliver this one again in a moment
    if (!uid) return { retry: true, message: `no membership for ${object.customer} yet` };
    const price = object.items && object.items.data && object.items.data[0] && object.items.data[0].price;
    const plan = planForPrice(env, price && price.id);
    const live = object.status === 'active' || object.status === 'trialing';
    await setPlan(env, uid, live && plan ? plan : 'bronze', object.customer);
    return `${uid} -> ${live && plan ? plan : 'bronze'} (${object.status})`;
  }

  if (event.type === 'customer.subscription.deleted') {
    const uid = await uidForCustomer(env, object.customer);
    if (!uid) return 'ignored: unknown customer';
    await setPlan(env, uid, 'bronze', object.customer);
    return `${uid} -> bronze (cancelled)`;
  }

  return `ignored: ${event.type}`;
}

export default {
  async fetch(request, env) {
    if (request.method === 'GET') return new Response('Chezz billing webhook is running.\n');
    if (request.method !== 'POST') return new Response('method not allowed', { status: 405 });

    const raw = await request.text();
    if (!(await verifyStripe(raw, request.headers.get('stripe-signature'), env.STRIPE_WEBHOOK_SECRET)))
      return new Response('bad signature', { status: 400 });

    let result;
    try {
      result = await handle(JSON.parse(raw), env);
    } catch (err) {
      // 500 makes Stripe try again; nothing is lost if Firestore or Google is briefly unreachable
      return new Response(`error: ${err.message}`, { status: 500 });
    }
    if (result && result.retry) return new Response(result.message, { status: 503 });
    return new Response(`ok: ${result}\n`);
  },
};
