// Typical bit-sdk-js usage from Node (>= 18).
//
//   BIT_URL=http://127.0.0.1:7777 BIT_KEY=your-key BIT_PWD=your-pwd \
//     node examples/node-example.mjs
//
// Contract: ../docs/API.md
import { BitClient } from '../index.js';

const client = new BitClient({
  baseUrl: process.env.BIT_URL || 'http://127.0.0.1:7777', // port shown in BIT settings
  clientKey: process.env.BIT_KEY || 'bit_test_key_123456',
  accessPassword: process.env.BIT_PWD,
});

const health = await client.health();
console.log('health:', JSON.stringify(health));

// Register a remote tool (BIT will POST tool calls back to this URL).
const name = `js_example_${Date.now()}`;
const tool = await client.registerTool({
  name,
  description: 'example tool registered by the JS SDK',
  parameters: { type: 'object', properties: { q: { type: 'string' } } },
  url: 'http://127.0.0.1:9000/hook',
});
console.log('registered:', tool.id, tool.name);

const result = await client.invokeTool(tool.id, { q: 'hi' });
console.log('invoked:', JSON.stringify(result));

// One full agent turn.
const chat = await client.chat({ message: 'hello from the JS SDK example' });
console.log('reply:', chat.reply);

// OpenAI-compatible streaming (password-exempt on /v1/*).
const full = await client.chatCompletionsStream(
  { model: 'ignored', messages: [{ role: 'user', content: '你好' }] },
  (delta) => {
    if (delta !== null) process.stdout.write(delta);
  }
);
console.log('\nstreamed text:', full);

// Clean up.
await client.removeTool(tool.id);
console.log('removed:', tool.id);
