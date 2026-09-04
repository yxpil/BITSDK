-- json.lua — minimal pure-Lua JSON encoder/decoder (Lua 5.4, zero dependencies).
--
-- Conventions:
--   * JSON objects   -> Lua tables with string keys
--   * JSON arrays    -> Lua tables with integer keys [1..n]
--   * Tables with a non-nil [1] encode as arrays; empty tables encode as {}
--     (an object). This means an empty JSON array round-trips as {} — keep
--     that in mind if you re-encode server data.
--   * JSON null decodes to nil (fields set to null simply disappear).
--
-- json.encode(value) -> string
-- json.decode(string) -> value   (raises a Lua error on malformed input;
--                                 wrap calls in pcall to recover)

local json = {}

-- ---------------------------------------------------------------- encoding

local ESCAPES = {
  ['"'] = '\\"',
  ['\\'] = '\\\\',
  ['\b'] = '\\b',
  ['\f'] = '\\f',
  ['\n'] = '\\n',
  ['\r'] = '\\r',
  ['\t'] = '\\t',
}

local function encode_string(s, out)
  out[#out + 1] = '"'
  -- byte-wise scan is UTF-8 safe: bytes >= 0x80 are passed through untouched
  for i = 1, #s do
    local c = s:sub(i, i)
    local e = ESCAPES[c]
    if e then
      out[#out + 1] = e
    elseif c < ' ' then
      out[#out + 1] = string.format('\\u%04x', c:byte())
    else
      out[#out + 1] = c
    end
  end
  out[#out + 1] = '"'
end

local function encode_value(v, out)
  local t = type(v)
  if v == nil then
    out[#out + 1] = 'null'
  elseif t == 'boolean' then
    out[#out + 1] = tostring(v)
  elseif t == 'number' then
    if v ~= v or v == math.huge or v == -math.huge then
      out[#out + 1] = 'null' -- NaN/inf are not representable in JSON
    else
      out[#out + 1] = tostring(v) -- Lua prints integers w/o '.0', floats shortest-round-trip
    end
  elseif t == 'string' then
    encode_string(v, out)
  elseif t == 'table' then
    if v[1] ~= nil then
      out[#out + 1] = '['
      for i = 1, #v do
        if i > 1 then out[#out + 1] = ',' end
        encode_value(v[i], out)
      end
      out[#out + 1] = ']'
    else
      out[#out + 1] = '{'
      local first = true
      for k, val in pairs(v) do
        if type(k) ~= 'string' then
          error('json.encode: non-string table key: ' .. tostring(k), 0)
        end
        if not first then out[#out + 1] = ',' end
        first = false
        encode_string(k, out)
        out[#out + 1] = ':'
        encode_value(val, out)
      end
      out[#out + 1] = '}'
    end
  else
    error('json.encode: unsupported type: ' .. t, 0)
  end
end

function json.encode(v)
  local out = {}
  encode_value(v, out)
  return table.concat(out)
end

-- ---------------------------------------------------------------- decoding

local DECODER = {}
DECODER.__index = DECODER

local WHITESPACE = { [32] = true, [9] = true, [10] = true, [13] = true }

local SIMPLE_ESCAPES = {
  [34] = '"',  -- \" (also terminates strings; harmless inside strings)
  [92] = '\\', -- \\
  [47] = '/',  -- \/
  [98] = '\b', -- \b
  [102] = '\f', -- \f
  [110] = '\n', -- \n
  [114] = '\r', -- \r
  [116] = '\t', -- \t
}

local function new_decoder(s)
  return setmetatable({ s = s, pos = 1, n = #s }, DECODER)
end

function DECODER:skip_ws()
  while self.pos <= self.n do
    if not WHITESPACE[self.s:byte(self.pos)] then break end
    self.pos = self.pos + 1
  end
end

function DECODER:fail(msg)
  error(string.format('json.decode: %s at offset %d', msg, self.pos), 0)
end

function DECODER:value()
  self:skip_ws()
  if self.pos > self.n then self:fail('unexpected end of input') end
  local b = self.s:byte(self.pos)
  if b == 123 then return self:object() end -- {
  if b == 91 then return self:array() end   -- [
  if b == 34 then return self:string() end  -- "
  if b == 116 then return self:literal('true', true) end
  if b == 102 then return self:literal('false', false) end
  if b == 110 then return self:literal('null', nil) end
  return self:number()
end

function DECODER:literal(text, value)
  if self.s:sub(self.pos, self.pos + #text - 1) ~= text then
    self:fail('invalid literal')
  end
  self.pos = self.pos + #text
  return value
end

function DECODER:object()
  self.pos = self.pos + 1 -- consume '{'
  local obj = {}
  self:skip_ws()
  if self.s:byte(self.pos) == 125 then -- }
    self.pos = self.pos + 1
    return obj
  end
  while true do
    self:skip_ws()
    if self.s:byte(self.pos) ~= 34 then self:fail('expected string key') end
    local key = self:string()
    self:skip_ws()
    if self.s:byte(self.pos) ~= 58 then self:fail("expected ':'") end
    self.pos = self.pos + 1
    obj[key] = self:value()
    self:skip_ws()
    local b = self.s:byte(self.pos)
    if b == 44 then -- ,
      self.pos = self.pos + 1
    elseif b == 125 then -- }
      self.pos = self.pos + 1
      return obj
    else
      self:fail("expected ',' or '}'")
    end
  end
end

function DECODER:array()
  self.pos = self.pos + 1 -- consume '['
  local arr = {}
  self:skip_ws()
  if self.s:byte(self.pos) == 93 then -- ]
    self.pos = self.pos + 1
    return arr
  end
  while true do
    arr[#arr + 1] = self:value()
    self:skip_ws()
    local b = self.s:byte(self.pos)
    if b == 44 then -- ,
      self.pos = self.pos + 1
    elseif b == 93 then -- ]
      self.pos = self.pos + 1
      return arr
    else
      self:fail("expected ',' or ']'")
    end
  end
end

function DECODER:string()
  self.pos = self.pos + 1 -- consume opening quote
  local s, n = self.s, self.n
  local buf = {}
  while true do
    if self.pos > n then self:fail('unterminated string') end
    local b = s:byte(self.pos)
    if b == 34 then -- closing quote
      self.pos = self.pos + 1
      return table.concat(buf)
    elseif b == 92 then -- backslash escape
      local e = s:byte(self.pos + 1)
      local simple = SIMPLE_ESCAPES[e]
      if simple then
        buf[#buf + 1] = simple
        self.pos = self.pos + 2
      elseif e == 117 then -- \uXXXX (with surrogate pair support)
        local hex = s:sub(self.pos + 2, self.pos + 5)
        local cp = #hex == 4 and tonumber(hex, 16) or nil
        if not cp then self:fail('invalid \\u escape') end
        self.pos = self.pos + 6
        if cp >= 0xD800 and cp <= 0xDBFF
            and s:byte(self.pos) == 92 and s:byte(self.pos + 1) == 117 then
          local hex2 = s:sub(self.pos + 2, self.pos + 5)
          local cp2 = #hex2 == 4 and tonumber(hex2, 16) or nil
          if cp2 and cp2 >= 0xDC00 and cp2 <= 0xDFFF then
            cp = 0x10000 + (cp - 0xD800) * 0x400 + (cp2 - 0xDC00)
            self.pos = self.pos + 6
          end
        end
        if cp >= 0xD800 and cp <= 0xDFFF then cp = 0xFFFD end -- lone surrogate
        buf[#buf + 1] = utf8.char(cp)
      else
        self:fail('invalid escape sequence')
      end
    else
      -- plain run: consume until quote, backslash or end of input
      local start = self.pos
      repeat
        self.pos = self.pos + 1
        b = s:byte(self.pos)
      until b == nil or b == 34 or b == 92
      buf[#buf + 1] = s:sub(start, self.pos - 1)
    end
  end
end

function DECODER:number()
  local s, pos = self.s, self.pos
  local num = s:match('^-?%d+%.?%d*[eE][-+]?%d+', pos)
      or s:match('^-?%d+%.?%d*', pos)
  if not num then self:fail('invalid number') end
  self.pos = pos + #num
  local v = tonumber(num)
  if v == nil then self:fail('invalid number') end
  return v
end

function json.decode(str)
  if type(str) ~= 'string' then
    error('json.decode: expected string, got ' .. type(str), 2)
  end
  local d = new_decoder(str)
  local value = d:value()
  d:skip_ws()
  if d.pos <= d.n then d:fail('trailing garbage') end
  return value
end

return json
