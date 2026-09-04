# bit-sdk-js

JavaScript SDK for the BIT HTTP API. Zero dependencies, Node >= 18.
The binding contract is [`../docs/API.md`](../docs/API.md).

## Install

```bash
npm i bit-sdk-js
# or directly from this repo:
npm i /path/to/BITSDK/javascript
```

Dual ESM/CJS: `import { BitClient } from 'bit-sdk-js'` and
`const { BitClient } = require('bit-sdk-js')` both work.

## Authentication

- Client Key: sent as `Authorization: Bearer <key>`, or as `?key=<key>` when
  the client is created with `keyInQuery: true` (for environments that cannot
  set headers).
- Access password: sent as `X-Access-Password` whenever configured. The server
  exempts `GET /api/health` (no auth at all), `/v1/*` and `/mcp` from the
  password check; sending the header there is harmless.
- `503` means BIT has no Client Key configured (remote access disabled).
  `401` means wrong key, or wrong/missing password.
- All failures throw `BitError` with `status`, `message` (parsed from the
  `error` field of the JSON body) and `raw` (parsed body). Network and timeout
  failures throw `BitError` with `status: 0` and message
  `network error: <reason>`.

## Usage

```js
import { BitClient } from 'bit-sdk-js';

const client = new BitClient({
  baseUrl: 'http://127.0.0.1:7777', // port shown in BIT settings
  clientKey: 'your-client-key',
  accessPassword: 'your-access-password', // optional
  timeoutMs: 30000, // optional
});

await client.health();
await client.registerTool({
  name: 'weather',
  description: 'query weather',
  parameters: { type: 'object', properties: {} },
  url: 'http://127.0.0.1:9000/hook',
});
const { reply } = await client.chat({ message: 'hello', sessionId: 'mysession' });
const text = await client.chatCompletionsStream(
  { messages: [{ role: 'user', content: '你好' }] },
  (delta, chunk) => { if (delta !== null) process.stdout.write(delta); },
); // text is the fully assembled message; the final callback gets (null, lastChunk with usage)
```

## Test

Start the fake server, then run the smoke test:

```bash
node test/fake_bit_server.js &
BIT_URL=http://127.0.0.1:9803 BIT_KEY=bit_test_key_123456 BIT_PWD=test-pwd-1 \
  node test/smoke.mjs
```

A runnable example lives in [`examples/node-example.mjs`](examples/node-example.mjs).
