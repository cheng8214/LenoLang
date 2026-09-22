local t = os.clock() * 1000
local result = 0.0
for i = 1, 50000000 do
    result = result + math.sin(i) * math.cos(i)
    result = result + math.sqrt(i)
    result = result + math.log(i)
end
print(result)
local t1 = os.clock() * 1000
print((t1 - t) .. "ms")
io.read()