--[[
The MIT License (MIT)

Copyright (c) 2014 Danila Poyarkov <dannotemail@gmail.com>
              2025 virtualgrub39   <virtualgrub39@tutamail.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
--]]

local template = {}

function template.escape(data)
  return tostring(data or ''):gsub("[\">/<'&]", {
    ["&"] = "&amp;",
    ["<"] = "&lt;",
    [">"] = "&gt;",
    ['"'] = "&quot;",
    ["'"] = "&#39;",
    ["/"] = "&#47;"
  })
end

function template.print(data, args, callback)
  local callback = callback or print

  local function exec(chunk)
    if type(chunk) == "function" then
      local env = args or {}
      setmetatable(env, { __index = _G })

      if _ENV then
        -- Lua 5.2+
        local wrapper, err = load([[
          return function(_ENV, exec, f)
            f(exec)
          end
        ]], "wrapper", "t", env)
        if not wrapper then error(err) end
        wrapper()(env, exec, chunk)

      else
        -- Lua 5.1
        setfenv(chunk, env)
        chunk(exec)
      end

    else
      callback(tostring(chunk or ""))
    end
  end

  exec(data)
end


function template.parse(data, minify)
  local str = 
    "return function(_)" .. 
      "function __(...)" ..
        "_(require('template').escape(...))" ..
      "end " ..
      "_[=[" ..
      data:
        gsub("[][]=[][]", ']=]_"%1"_[=['):
        gsub("<%%=", "]=]_("):
        gsub("<%%", "]=]__("):
        gsub("%%>", ")_[=["):
        gsub("<%?", "]=] "):
        gsub("%?>", " _[=[") ..
      "]=] " ..
    "end"
  if minify then
    str = str:
      gsub("^[ %s]*", ""):
      gsub("[ %s]*$", ""):
      gsub("%s+", " ")
  end
  return str
end

function template.compile(...)
  local f, err = load(template.parse(...))
  if err then
    error(err)
  end
  return f()
end

function template.render(src, args)
  local env = args or {}
  setmetatable(env, { __index = _G })

  local chunk_src = template.parse(src)

  local tpl_chunk, err = load(chunk_src, "template", "t", env)
  if not tpl_chunk then return nil, err end

  local renderer = tpl_chunk()

  local parts = {}
  local function collect(str)
    parts[#parts + 1] = tostring(str or "")
  end

  renderer(collect)
  return table.concat(parts)
end

function template.render_file(path, args)
  local f, err = io.open(path, "rb")
  if not f then
    return nil, err
  end
  local src = f:read("*a")
  f:close()
  return template.render(src, args)
end

return template