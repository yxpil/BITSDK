-- Example: using the Lua BIT SDK.
--
-- Run from the lua/ directory:   lua examples/example.lua
-- Or from anywhere:              lua lua/examples/example.lua
--
-- Env (defaults target the fake test server):
--   BIT_URL (default http://127.0.0.1:9803)
--   BIT_KEY (default bit_test_key_123456)
--   BIT_PWD (default test-pwd-1)

local script_dir = arg[0]:match('^(.*)[/\\]') or '.'
package.path = script_dir .. '/?.lua;' .. script_dir .. '/../?.lua;' .. package.path

local BitClient = require('bitsdk')

local url = os.getenv('BIT_URL') or 'http://127.0.0.1:9803'
local key = os.getenv('BIT_KEY') or 'bit_test_key_123456'
local pwd = os.getenv('BIT_PWD') or 'test-pwd-1'

local bit = BitClient.new(url, key, {
  access_password = pwd,   -- sent as X-Access-Password on /api/* paths
  timeout_ms = 30000,
})

-- 1) Health — no auth needed.
local health = bit:health()
if not health then error('health check failed') end
print('health: ok=' .. tostring(health.ok) .. ' version=' .. tostring(health.version))

-- 2) Register -> invoke -> remove a remote tool.
local name = 'example_tool_' .. os.time()
local reg, err = bit:register_tool{
  name = name,
  description = 'created by the lua example',
  parameters = { type = 'object', properties = {} },
  url = 'http://127.0.0.1:9000/hook',
}
if not reg then error('register failed: ' .. tostring(err.message)) end
local id = reg.tool.id
print('registered tool: ' .. id)

local inv = bit:invoke_tool(id, { text = 'hi' })
if inv then print('invoked via: ' .. inv.result.via) end

local rem = bit:remove_tool(id)
if rem then print('removed: ' .. rem.removed) end

-- 3) Agent chat.
local chat, cerr = bit:chat('hello from lua sdk')
if chat then
  print('reply: ' .. chat.reply)
else
  print('chat failed: ' .. tostring(cerr.message))
end

-- 4) OpenAI-compatible streaming (access password exempt).
--    on_delta(text, nil) per content chunk; on_delta(nil, final) for the
--    last parsed chunk (may carry usage).
local text, serr = bit:chat_completions_stream(
  { model = 'bit', messages = { { role = 'user', content = '你好' } } },
  function(delta, fin)
    if delta then io.write(delta) end
    if fin and fin.usage then
      io.write('\n[stream done] usage total_tokens=', tostring(fin.usage.total_tokens), '\n')
    end
  end
)
if not text then
  io.stderr:write('stream failed: ', tostring(serr.message), '\n')
else
  print('full text: ' .. text)
end
