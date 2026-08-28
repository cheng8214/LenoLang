import time

N = 10000000

def f(x):
    for _ in range(1):
        pass
    return x

def g(x):
    for _ in range(1):
        pass
    return x

t1 = time.perf_counter()
s = 0
for _ in range(N):
    s = f(s)
t2 = time.perf_counter()
print(f"int调用: {(t2-t1)*1000:.0f}ms")

t1 = time.perf_counter()
d = 0.0
for _ in range(N):
    d = g(d)
t2 = time.perf_counter()
print(f"float调用: {(t2-t1)*1000:.0f}ms")
