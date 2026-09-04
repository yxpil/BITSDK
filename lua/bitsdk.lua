-- bitsdk.lua — Lua SDK for the BIT HTTP API (see ../docs/API.md).
--
-- Lua 5.4, zero dependencies: HTTP transport is a `curl` subprocess and JSON
-- is handled by the bundled pure-Lua json.lua sibling module.
--
-- Usage:
--   local BitClient = require('bitsdk')
--   local bit = BitClient.new('http://127.0.0.1:7777', 'YOUR_CLIENT_KEY',
--                             { access_password = '...', timeout_ms = 30000 })
--   local health = bit:health()             -- returns parsed table on success
--   if not health then ... end              -- errors: see below
--
-- Error handling (all methods): on success returns (result_table); on failure
-- returns (nil, err) where err is:
--   { status = <http status, 0 for transport failures>,
--     message = "<server error text, or English transport message>",
--     raw = "<raw response body>" }
--
-- Auth (mirrors docs/API.md):
--   * /api/health — no auth.
--   * /api/*      — Client Key (Authorization: Bearer, or ?key= when
--                   key_in_query is set) + X-Access-Password.
--   * /v1/*, /mcp — Client Key only (access password exempt).

local json = require('json')

local BitClient = {}
BitClient.__index = BitClient

local DEFAULT_TIMEOUT_MS = 30000
local DEFAULT_CURL = 'curl'

-- ------------------------------------------------------------------ helpers

local function shell_quote(s)
  return "'" .. tostring(s):gsub("'", "'\\''") .. "'"
end

local function percent_encode(s)
  return (tostring(s):gsub('[^%w%-_%.~]', function(c)
    return string.format('%%%02X', c:byte())
  end))
end

local function write_tmpfile(content)
  local path = os.tmpname()
  local f = assert(io.open(path, 'wb'))
  f:write(content)
  f:close()
  return path
end

local function read_tmpfile(path)
  local f = io.open(path, 'rb')
  if not f then return '' end
  local data = f:read('*a') or ''
  f:close()
  return data
end

-- Extract the human-readable message from a BIT error body, which is always
-- JSON with an "error" field: a plain string, or an OpenAI-style object
-- carrying a "message" member.
local function error_message_from(raw)
  if type(raw) ~= 'string' or raw == '' then return nil end
  local ok, parsed = pcall(json.decode, raw)
  if ok and type(parsed) == 'table' and parsed.error ~= nil then
    if type(parsed.error) == 'string' then
      return parsed.error
    elseif type(parsed.error) == 'table' then
      if type(parsed.error.message) == 'string' then
        return parsed.error.message
      end
      return json.encode(parsed.error)
    end
  end
  return nil
end

local function new_err(status, message, raw)
  return { status = status, message = message, raw = raw or '' }
end

-- -------------------------------------------------------------------- class

--- Create a client. opts (all optional):
---   access_password : string  — sent as X-Access-Password on /api/* paths
---   timeout_ms      : number  — per-request timeout (default 30000)
---   key_in_query    : boolean — send the key as ?key= instead of a header
---   curl_path       : string  — path to the curl binary (default "curl")
function BitClient.new(base_url, client_key, opts)
  opts = opts or {}
  local self = setmetatable({}, BitClient)
  self.base_url = tostring(base_url or ''):gsub('/+$', '')
  self.client_key = tostring(client_key or '')
  self.access_password = opts.access_password
  self.timeout_ms = tonumber(opts.timeout_ms) or DEFAULT_TIMEOUT_MS
  self.key_in_query = opts.key_in_query and true or false
  self.curl_path = tostring(opts.curl_path or DEFAULT_CURL)
  return self
end

function BitClient:_build_url(path)
  local url = self.base_url .. path
  if self.key_in_query and self.client_key ~= '' then
    url = url .. '?key=' .. percent_encode(self.client_key)
  end
  return url
end

-- Build curl arguments for an authenticated request.
-- need_password: /api/* requires X-Access-Password; /v1/* and /mcp do not.
function BitClient:_auth_args(args, need_password)
  if self.client_key ~= '' and not self.key_in_query then
    args[#args + 1] = '-H'
    args[#args + 1] = shell_quote('Authorization: Bearer ' .. self.client_key)
  end
  if need_password and self.access_password then
    args[#args + 1] = '-H'
    args[#args + 1] = shell_quote('X-Access-Password: ' .. self.access_password)
  end
end

local function curl_common_args(self)
  return {
    shell_quote(self.curl_path),
    '-s',                          -- silent: no progress/errors on stderr
    '--max-time', string.format('%.2f', self.timeout_ms / 1000),
  }
end

-- Run one HTTP request. Returns:
--   status_code, body_text        on any completed HTTP exchange
--   nil, transport_error_message  when curl could not complete
function BitClient:_request(method, path, raw_body, need_password)
  local args = curl_common_args(self)
  args[#args + 1] = '-X'
  args[#args + 1] = shell_quote(method)
  self:_auth_args(args, need_password)

  local body_file = nil
  if raw_body ~= nil then
    args[#args + 1] = '-H'
    args[#args + 1] = shell_quote('Content-Type: application/json')
    body_file = write_tmpfile(raw_body)
    args[#args + 1] = '-d'
    args[#args + 1] = '@- < ' .. shell_quote(body_file) -- feed body via stdin
  end

  local out_file = os.tmpname()
  args[#args + 1] = '-o'
  args[#args + 1] = shell_quote(out_file)
  args[#args + 1] = '-w'
  args[#args + 1] = shell_quote('%{http_code}') -- status code on stdout
  args[#args + 1] = shell_quote(self:_build_url(path))

  local pipe = io.popen(table.concat(args, ' '), 'r')
  local status_out
  if pipe then
    status_out = pipe:read('*a') or ''
    pipe:close()
  else
    status_out = ''
  end
  local body = read_tmpfile(out_file)
  os.remove(out_file)
  if body_file then os.remove(body_file) end

  local status = tonumber((status_out:match('^%s*(.-)%s*$')))
  if status == nil or status == 0 then
    return nil, 'curl failed: no HTTP response (unreachable server, refused connection or timeout)'
  end
  return status, body
end

-- Core call used by every non-streaming method. Returns (parsed_table) or
-- (nil, err_table).
function BitClient:_call(method, path, body_table, need_password)
  local raw_body = body_table ~= nil and json.encode(body_table) or nil
  local status, body = self:_request(method, path, raw_body, need_password)
  if status == nil then
    return nil, new_err(0, body) -- body carries the transport error message
  end
  if status < 200 or status >= 300 then
    local msg = error_message_from(body) or ('HTTP ' .. status)
    return nil, new_err(status, msg, body)
  end
  if body == nil or body == '' then return {} end
  local ok, parsed = pcall(json.decode, body)
  if not ok then
    return nil, new_err(status, 'invalid JSON in response body', body)
  end
  return parsed
end

-- ----------------------------------------------------------------- /api/*

--- GET /api/health — no auth required.
function BitClient:health()
  return self:_call('GET', '/api/health', nil, false)
end

--- GET /api/tools.
function BitClient:list_tools()
  return self:_call('GET', '/api/tools', nil, true)
end

--- POST /api/tools — register a remote tool.
--- t = { name=..., description=..., parameters={...} or JSON string, url=... }
function BitClient:register_tool(t)
  t = t or {}
  local parameters = t.parameters or { type = 'object', properties = {} }
  if type(parameters) == 'string' then
    local ok, decoded = pcall(json.decode, parameters)
    if not ok then
      return nil, new_err(0, 'register_tool: parameters is not valid JSON: ' .. tostring(decoded))
    end
    parameters = decoded
  end
  local body = {
    name = t.name,
    description = t.description or '',
    parameters = parameters,
    url = t.url,
  }
  return self:_call('POST', '/api/tools', body, true)
end

--- DELETE /api/tools/{id}.
function BitClient:remove_tool(id)
  return self:_call('DELETE', '/api/tools/' .. percent_encode(tostring(id)), nil, true)
end

--- POST /api/tools/{id}/invoke.
function BitClient:invoke_tool(id, params)
  local path = '/api/tools/' .. percent_encode(tostring(id)) .. '/invoke'
  return self:_call('POST', path, { params = params or {} }, true)
end

--- POST /api/chat — run one agent turn.
--- chat(message, session_id, images): session_id and images may be nil.
function BitClient:chat(message, session_id, images)
  local body = { message = message }
  if session_id ~= nil then body.session_id = session_id end
  if images ~= nil then body.images = images end
  return self:_call('POST', '/api/chat', body, true)
end

--- Table-argument variant: chat_table{ message=..., session_id=..., images=... }.
function BitClient:chat_table(t)
  t = t or {}
  return self:chat(t.message, t.session_id, t.images)
end

--- GET /api/audit.
function BitClient:audit()
  return self:_call('GET', '/api/audit', nil, true)
end

--- GET /api/debug/state.
function BitClient:debug_state()
  return self:_call('GET', '/api/debug/state', nil, true)
end

--- GET /api/debug/sessions.
function BitClient:debug_sessions()
  return self:_call('GET', '/api/debug/sessions', nil, true)
end

--- GET /api/debug/sessions/{id}.
function BitClient:debug_session(id)
  return self:_call('GET', '/api/debug/sessions/' .. percent_encode(tostring(id)), nil, true)
end

--- GET /api/debug/mcp.
function BitClient:debug_mcp()
  return self:_call('GET', '/api/debug/mcp', nil, true)
end

-- -------------------------------------------------------------- /mcp, /v1/*

--- POST /mcp — raw JSON-RPC 2.0 payload (as a Lua table). Password exempt.
function BitClient:mcp(payload)
  return self:_call('POST', '/mcp', payload, false)
end

--- GET /v1/models. Password exempt.
function BitClient:models()
  return self:_call('GET', '/v1/models', nil, false)
end

--- POST /v1/chat/completions (non-streaming OpenAI call). Password exempt.
function BitClient:chat_completions(body)
  return self:_call('POST', '/v1/chat/completions', body, false)
end

--- POST /v1/chat/completions with stream=true, reading the SSE response line
--- by line (safe for multi-byte UTF-8 split across TCP chunks, because we
--- only ever split on '\n').
---
--- on_delta(chunk_text, nil) is called per content chunk;
--- on_delta(nil, final_table) is called once for the last parsed chunk
--- before [DONE] (which may carry "usage").
---
--- Returns the assembled full text, or (nil, err_table).
function BitClient:chat_completions_stream(body, on_delta)
  local payload = {}
  for k, v in pairs(body or {}) do payload[k] = v end
  payload.stream = true

  local req_file = write_tmpfile(json.encode(payload))
  local args = curl_common_args(self)
  args[#args + 1] = '-N' -- --no-buffer: stream as it arrives
  args[#args + 1] = '-X'
  args[#args + 1] = shell_quote('POST')
  self:_auth_args(args, false) -- /v1/* is password exempt
  args[#args + 1] = '-H'
  args[#args + 1] = shell_quote('Content-Type: application/json')
  args[#args + 1] = '-d'
  args[#args + 1] = '@- < ' .. shell_quote(req_file) -- feed body via stdin
  args[#args + 1] = '-w'
  args[#args + 1] = shell_quote('\n%{http_code}') -- status after the stream
  args[#args + 1] = shell_quote(self:_build_url('/v1/chat/completions'))

  local pipe = io.popen(table.concat(args, ' '), 'r')
  if not pipe then
    os.remove(req_file)
    return nil, new_err(0, 'curl failed: cannot spawn curl')
  end

  local text_parts = {}
  local raw_lines = {}
  local last_chunk = nil
  local saw_data = false
  local http_status = nil

  for line in pipe:lines() do
    raw_lines[#raw_lines + 1] = line
    if line:sub(1, 5) == 'data:' then
      saw_data = true
      local data = line:sub(6):gsub('^%s+', ''):gsub('%s+$', '')
      if data ~= '[DONE]' then
        local ok, chunk = pcall(json.decode, data)
        if ok and type(chunk) == 'table' then
          last_chunk = chunk
          local content = nil
          local choices = chunk.choices
          if type(choices) == 'table' and type(choices[1]) == 'table' then
            local delta = choices[1].delta
            if type(delta) == 'table' and type(delta.content) == 'string' then
              content = delta.content
            end
          end
          if content ~= nil then
            text_parts[#text_parts + 1] = content
            if on_delta then on_delta(content, nil) end
          end
        end
      end
    elseif line:match('^%d%d%d$') then
      http_status = tonumber(line) -- trailing -w output after the stream
    end
  end
  pipe:close()
  os.remove(req_file)

  local raw = table.concat(raw_lines, '\n')
  if http_status == nil then
    return nil, new_err(0, 'curl failed: no HTTP response (unreachable server, refused connection or timeout)', raw)
  end
  if http_status < 200 or http_status >= 300 then
    local msg = error_message_from(raw) or ('HTTP ' .. http_status)
    return nil, new_err(http_status, msg, raw)
  end
  if not saw_data and last_chunk == nil then
    local msg = error_message_from(raw)
    if msg then
      return nil, new_err(http_status, msg, raw)
    end
  end
  if on_delta and last_chunk then
    on_delta(nil, last_chunk)
  end
  return table.concat(text_parts, '')
end

return BitClient
