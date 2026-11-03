// Exercise the actual inline worker without a browser or a running emulator.
const fs = require('fs'), vm = require('vm'), assert = require('assert');
const html = fs.readFileSync(process.argv[2] || 'fastlink-spectra/prototype.html', 'utf8');
const source = html.match(/const source = `([\s\S]*?)`;/)[1]
  .replace('${JSON.stringify(RAM_URL)}', JSON.stringify('http://127.0.0.1:9000/'))
  .replace('${POLL_MS}', '100');
let now = 0, nextId = 0, requests = 0, size = 131072, hanging = false;
const timers = new Map(), messages = [];
const context = {
  performance: { now: () => now }, AbortController,
  setTimeout(callback, delay) { const id = ++nextId; timers.set(id, { at: now + delay, callback }); return id; },
  clearTimeout(id) { timers.delete(id); },
  async fetch(url, options) {
    requests++;
    assert.strictEqual(url, 'http://127.0.0.1:9000/');
    if(hanging) return new Promise((resolve, reject) => options.signal.addEventListener('abort', () => reject(new Error('aborted'))));
    return { ok: true, async arrayBuffer() { return new ArrayBuffer(size); } };
  },
  postMessage(message) { messages.push(message); }
};
vm.createContext(context);
vm.runInContext(source, context);
async function flush() { for(let i = 0; i < 12; i++) await Promise.resolve(); }
async function advance() {
  assert(timers.size);
  const [id, timer] = [...timers].sort((a,b) => a[1].at - b[1].at)[0];
  timers.delete(id); now = timer.at; timer.callback(); await flush();
}
function send(data) { context.onmessage({ data }); }
(async () => {
  await flush();
  assert.strictEqual(requests, 1); assert.strictEqual(messages[0].type, 'ram');
  await advance(); // timeout while main thread is busy; no second snapshot queues
  assert.strictEqual(requests, 1); assert.strictEqual(messages.length, 1);
  send({ ack: true }); await flush(); await advance();
  assert.strictEqual(requests, 2);
  send({ active: false }); send({ ack: true }); await flush();
  for(let i = 0; i < 8; i++) await advance();
  assert.strictEqual(requests, 2);
  size = 64; send({ active: true }); await advance();
  assert.strictEqual(messages.at(-1).type, 'offline');
  hanging = true; send({ ack: true }); await flush(); await advance();
  const count = messages.length;
  await advance();
  assert.strictEqual(messages.length, count + 1);
  assert.strictEqual(messages.at(-1).type, 'offline');
  console.log('PASS: one pending snapshot, acknowledgement backpressure, hidden polling suspension, malformed response rejection, hung fetch timeout');
})().catch(error => { console.error(error); process.exitCode = 1; });
