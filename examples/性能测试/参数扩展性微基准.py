import time

N = 10000000

def f1(a):
    for _ in range(1):
        pass
    return a

def f7(a, b, c, d, e, f, g):
    for _ in range(1):
        pass
    return a

t1 = time.perf_counter()
s = 0
for _ in range(N):
    s = f1(s)
t2 = time.perf_counter()
print(f"1参调用: {(t2-t1)*1000:.0f}ms")

t1 = time.perf_counter()
for _ in range(N):
    s = f7(1, 2, 3, 4, 5, 6, s)
t2 = time.perf_counter()
print(f"7参调用: {(t2-t1)*1000:.0f}ms")
