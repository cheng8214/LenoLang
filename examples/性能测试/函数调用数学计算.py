import time
import math

t = time.perf_counter()
result = 0.0
for i in range(1, 50000001):
    result += math.sin(i) * math.cos(i)
    result += math.sqrt(i)
    result += math.log(i)
print(result)
t1 = time.perf_counter()
print(f"{(t1 - t) * 1000:.0f}ms")
