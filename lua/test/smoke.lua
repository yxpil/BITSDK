-- Smoke test for the Lua BIT SDK against the fake BIT server.
--
-- Start the server first:   node test/fake_bit_server.js   (port 9803)
-- Run:                      cd lua && lua test/smoke.lua
--
-- Env overrides: BIT_URL / BIT_KEY / BIT_PWD
-- (defaults match the fake server: http://127.0.0.1:9803,
--  bit_test_key_123456, test-pwd-1)
--
-- NOTE: the fake server is shared between concurrent test runs — this suite
-- only asserts on objects it created itself (plus fixed endpoints), never on
-- global tool/audit counts.

local script_dir = arg[0]:match('^(.*)[/\\]') or '.'
package.path = script_dir .. '/?.lua;' .. script_dir .. '/../?.lua;' .. package.path

local BitClient = require('bitsdk')

local BIT_URL = os.getenv('BIT_URL') or 'http://127.0.0.1:9803'
local BIT_KEY = os.getenv('BIT_KEY') or 'bit_test_key_123456'
local BIT_PWD = os.getenv('BIT_PWD') or 'test-pwd-1'

local passed, failed = 0, {}

local function check(name, ok, detail)
  if ok then
    passed = passed + 1
    print('[PASS] ' .. name)
  else
    failed[#failed + 1] = name
    local suffix = ''
    if detail ~= nil and detail ~= '' then suffix = ' - ' .. tostring(detail) end
    print('[FAIL] ' .. name .. suffix)
  end
end

local client = BitClient.new(BIT_URL, BIT_KEY, { access_password = BIT_PWD })

local res, err

-- 1. health (no auth required)
res, err = client:health()
check('health ok=true', res ~= nil and res.ok == true, err and err.message)

-- 2. tools lifecycle: register -> invoke -> remove (unique name: shared server)
local tool_name = string.format('lua_sdk_tool_%d_%d', os.time(), math.random(1000, 9999))
res, err = client:register_tool{
  name = tool_name,
  description = 'created by lua smoke test',
  parameters = { type = 'object', properties = {} },
  url = 'http://127.0.0.1:9803/hook',
}
local tool_id = res and res.tool and res.tool.id or nil
check('register tool returns id', tool_id ~= nil and tool_id ~= '', err and err.message)

res, err = client:invoke_tool(tool_id, { hello = 'lua' })
check('invoke tool result.via==name',
  res ~= nil and res.result ~= nil and res.result.via == tool_name
  and res.result.echoed ~= nil and res.result.echoed.hello == 'lua',
  err and err.message)

local removed
removed, err = client:remove_tool(tool_id)
check('remove tool removed==id', removed ~= nil and removed.removed == tool_id, err and err.message)

-- 3. chat
res, err = client:chat('lua smoke ' .. os.time())
check('chat reply prefix "fake reply to:"',
  res ~= nil and type(res.reply) == 'string' and res.reply:sub(1, #'fake reply to:') == 'fake reply to:',
  err and err.message)

-- 4. chat_table variant
res, err = client:chat_table{ message = 'lua table chat' }
check('chat_table reply prefix "fake reply to:"',
  res ~= nil and type(res.reply) == 'string' and res.reply:sub(1, #'fake reply to:') == 'fake reply to:',
  err and err.message)

-- 5. debug_state
res, err = client:debug_state()
check('debug_state.ai.active.model exists',
  res ~= nil and type(res.ai) == 'table' and type(res.ai.active) == 'table'
  and res.ai.active.model ~= nil and res.ai.active.model ~= '',
  err and err.message)

-- 6. debug_sessions (>=1; shared server — no exact count)
res, err = client:debug_sessions()
check('debug_sessions >=1', res ~= nil and type(res.sessions) == 'table' and #res.sessions >= 1,
  err and err.message)

-- 7. debug_session('default')
res, err = client:debug_session('default')
check("debug_session('default') has messages",
  res ~= nil and type(res.messages) == 'table' and #res.messages >= 1,
  err and err.message)

-- 8. debug_mcp
res, err = client:debug_mcp()
check('debug_mcp returns tools', res ~= nil and type(res.tools) == 'table', err and err.message)

-- 9. audit (structure only; entries are shared global state)
res, err = client:audit()
check('audit returns entries', res ~= nil and type(res.entries) == 'table', err and err.message)

-- 10. mcp initialize
res, err = client:mcp{
  jsonrpc = '2.0', id = 1, method = 'initialize',
  params = {
    protocolVersion = '2025-03-26',
    capabilities = {},
    clientInfo = { name = 'lua-sdk', version = '1.0' },
  },
}
check('mcp initialize result.serverInfo.name=="fake-bit"',
  res ~= nil and type(res.result) == 'table' and type(res.result.serverInfo) == 'table'
  and res.result.serverInfo.name == 'fake-bit',
  err and err.message)

-- 11. models (fixed endpoint: exactly 2 models)
res, err = client:models()
check('models data len 2', res ~= nil and type(res.data) == 'table' and #res.data == 2,
  err and err.message)

-- 12. chat_completions non-streaming (password exempt)
res, err = client:chat_completions{
  model = 'fake-model',
  messages = { { role = 'user', content = 'hi' } },
}
check('chat_completions content=="你好，世界!"',
  res ~= nil and type(res.choices) == 'table' and type(res.choices[1]) == 'table'
  and type(res.choices[1].message) == 'table' and res.choices[1].message.content == '你好，世界!',
  err and err.message)

-- 13. streaming: server chunks 你好 / ，世 / 界! — exercises multi-byte-safe
--     line splitting (chunks are split mid-character across TCP writes).
local parts = {}
local final_chunk = nil
local text
text, err = client:chat_completions_stream(
  { model = 'fake-model', messages = { { role = 'user', content = 'hi' } } },
  function(delta, fin)
    if delta then parts[#parts + 1] = delta end
    if fin then final_chunk = fin end
  end
)
local assembled = table.concat(parts, '')
check('stream assembles "你好，世界!"', text == '你好，世界!' and assembled == '你好，世界!',
  err and err.message)
check('stream final chunk has usage',
  final_chunk ~= nil and type(final_chunk.usage) == 'table' and final_chunk.usage.total_tokens == 17)

-- 14. wrong key -> 401
local bad = BitClient.new(BIT_URL, 'bit_wrong_key', { access_password = BIT_PWD })
res, err = bad:list_tools()
check('wrong key -> 401', res == nil and err ~= nil and err.status == 401,
  err and (tostring(err.status) .. ' ' .. tostring(err.message)))

-- 15. wrong/missing password -> 401
local nopwd = BitClient.new(BIT_URL, BIT_KEY, { access_password = 'totally-wrong' })
res, err = nopwd:list_tools()
check('missing password -> 401', res == nil and err ~= nil and err.status == 401,
  err and (tostring(err.status) .. ' ' .. tostring(err.message)))

-- 16. key_in_query (?key= instead of Authorization header)
local inq = BitClient.new(BIT_URL, BIT_KEY, { access_password = BIT_PWD, key_in_query = true })
res, err = inq:list_tools()
check('key_in_query works', res ~= nil and type(res.tools) == 'table', err and err.message)

-- summary
print(string.format('\n%d passed, %d failed', passed, #failed))
if #failed > 0 then
  for _, name in ipairs(failed) do
    print('  FAILED: ' .. name)
  end
  os.exit(1)
end
