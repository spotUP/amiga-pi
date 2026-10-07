-- A5 probe script: the same text must print the same bytes on host and Amiga.
local function counter()
  local n = 0
  return function() n = n + 1; return n end
end
local c = counter(); c(); c()
print("closure", c())

local t = {}
for i = 1, 10 do t[#t + 1] = i * i end
print("table", #t, t[1], t[10], table.concat(t, ",", 1, 5))

print("string", ("Amiga"):upper(), ("x"):rep(3), ("hello world"):find("wor"), ("a,b,c"):gsub(",", ";"))
print(string.format("%d %s %.2f", 42, "pi", 3.14159))

local ok, err = pcall(error, "boom")
print("pcall", ok, err)
local ok2, err2 = pcall(function() local x = nil; return x.y end)
print("pcall2", ok2, (tostring(err2):gsub("^.-:%d+: ", "")))

local q = {}
for i = 1, 100000 do table.insert(q, i) end
local sum = 0
for i = 1, 100000 do sum = sum + table.remove(q) end
print("loop", sum, #q)

local function fib(n) if n < 2 then return n end return fib(n - 1) + fib(n - 2) end
print("fib", fib(24))
