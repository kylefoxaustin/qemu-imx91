-- Lua self-test: deterministic assertions across core subsystems. Any failure
-- raises (nonzero exit); success prints the sentinel run.sh greps for.
local function eq(a, b, msg) assert(a == b, (msg or "") .. " expected " .. tostring(b) .. " got " .. tostring(a)) end

-- arithmetic / integer-float divide (5.3+ semantics)
eq(7 // 2, 3, "floordiv"); eq(7 % 3, 1, "mod"); eq(2^10, 1024.0, "pow")
eq(math.type(3), "integer", "inttype"); eq(math.type(3.0), "float", "floattype")
eq(math.maxinteger + 1, math.mininteger, "intwrap")

-- strings
eq(("hello"):upper(), "HELLO", "upper")
eq(string.format("%05.2f", 3.14159), "03.14", "fmt")
eq(#("abcdef"):gsub("%a", "x"), 6, "gsublen")
eq(select(2, ("a,b,c"):gsub(",", ";")), 2, "gsubcount")
eq(tostring(0x1p4), "16.0", "hexfloat")

-- tables / sort / iteration
local t = {5,3,1,4,2}; table.sort(t)
eq(table.concat(t, ","), "1,2,3,4,5", "sort")
local sum = 0; for _, v in ipairs(t) do sum = sum + v end; eq(sum, 15, "ipairs")

-- closures
local function counter() local n = 0; return function() n = n + 1; return n end end
local c = counter(); c(); c(); eq(c(), 3, "closure")

-- coroutines
local function gen(n) for i = 1, n do coroutine.yield(i * i) end end
local co = coroutine.wrap(function() gen(4) end)
eq(co() + co() + co() + co(), 1 + 4 + 9 + 16, "coroutine")

-- pcall / error
local ok, err = pcall(function() error("boom") end)
eq(ok, false, "pcall"); assert(err:match("boom"), "errmsg")

print("LUA SELFTEST OK")
