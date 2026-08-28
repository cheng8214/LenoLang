import time
import math

# 光线追踪对比测试（标量优化版，SoA 布局，无对象创建）
# 三段式：
#   Phase A: 数组索引微基准
#   Phase B: sphere_hit 多返回值调用微基准
#   Phase C: 完整递归 trace 光线追踪

NS = 10
NUM_RAYS = 300000
HITCALLS = 3000000

scx = []
scy = []
scz = []
sr = []


def init_scene():
    for i in range(NS):
        scx.append(i * 2.0 - 9.0)
        scy.append(((i * 37) % 11 - 5) * 0.7)
        scz.append(12.0 + (i % 3) * 4.0)
        sr.append(1.0 + (i % 3) * 0.5)


# ==================== Phase A: 数组索引微基准 ====================
def bench_index():
    t1 = time.perf_counter()
    sum_val = 0.0
    si = 0
    for _ in range(HITCALLS):
        sum_val += scx[si] + scy[si] + scz[si] + sr[si]
        si += 1
        if si >= NS:
            si = 0
    t2 = time.perf_counter()
    print(f"Phase A 数组索引: {HITCALLS}次循环(4次索引/轮): {(t2 - t1) * 1000:.0f}ms (sum={sum_val})")


# ==================== Phase B: sphere_hit 多返回值调用 ====================
def sphere_hit(ox, oy, oz, dx, dy, dz, si):
    ocx = ox - scx[si]
    ocy = oy - scy[si]
    ocz = oz - scz[si]
    r = sr[si]
    b = ocx * dx + ocy * dy + ocz * dz
    c = ocx * ocx + ocy * ocy + ocz * ocz - r * r
    disc = b * b - c
    if disc < 0.0:
        return -1.0, 0
    sq = math.sqrt(disc)
    t = -b - sq
    if t > 0.001:
        return t, 1
    t = -b + sq
    if t > 0.001:
        return t, 1
    return -1.0, 0


def bench_hit():
    t1 = time.perf_counter()
    hits = 0
    si = 0
    ox = 0.0
    oy = 0.0
    oz = 0.0
    dx = 0.15
    dy = 0.1
    dz = 1.0
    for _ in range(HITCALLS):
        t, hit = sphere_hit(ox, oy, oz, dx, dy, dz, si)
        hits += hit
        si += 1
        if si >= NS:
            si = 0
    t2 = time.perf_counter()
    print(f"Phase B sphere_hit多返回值: {HITCALLS}次调用: {(t2 - t1) * 1000:.0f}ms (hits={hits})")


# 单返回值版：t < 0 表示未命中，用于隔离"多返回值打包"的开销
def sphere_hit_single(ox, oy, oz, dx, dy, dz, si):
    ocx = ox - scx[si]
    ocy = oy - scy[si]
    ocz = oz - scz[si]
    r = sr[si]
    b = ocx * dx + ocy * dy + ocz * dz
    c = ocx * ocx + ocy * ocy + ocz * ocz - r * r
    disc = b * b - c
    if disc < 0.0:
        return -1.0
    sq = math.sqrt(disc)
    t = -b - sq
    if t > 0.001:
        return t
    t = -b + sq
    if t > 0.001:
        return t
    return -1.0


def bench_hit_single():
    t1 = time.perf_counter()
    hits = 0
    si = 0
    ox = 0.0
    oy = 0.0
    oz = 0.0
    dx = 0.15
    dy = 0.1
    dz = 1.0
    for _ in range(HITCALLS):
        t = sphere_hit_single(ox, oy, oz, dx, dy, dz, si)
        if t > 0.0:
            hits += 1
        si += 1
        if si >= NS:
            si = 0
    t2 = time.perf_counter()
    print(f"Phase B2 sphere_hit单返回值: {HITCALLS}次调用: {(t2 - t1) * 1000:.0f}ms (hits={hits})")


# ==================== Phase C: 递归 trace 光线追踪 ====================
def trace(ox, oy, oz, dx, dy, dz, depth):
    if depth <= 0:
        return 0
    tmin = 1000000000.0
    hit_idx = -1
    for si in range(NS):
        t, hit = sphere_hit(ox, oy, oz, dx, dy, dz, si)
        if hit == 1:
            if t < tmin:
                tmin = t
                hit_idx = si
    if hit_idx < 0:
        return 0
    # 命中点
    hx = ox + dx * tmin
    hy = oy + dy * tmin
    hz = oz + dz * tmin
    # 法线（除以半径）
    r = sr[hit_idx]
    nx = (hx - scx[hit_idx]) / r
    ny = (hy - scy[hit_idx]) / r
    nz = (hz - scz[hit_idx]) / r
    # 反射方向
    dot = dx * nx + dy * ny + dz * nz
    rdx = dx - 2.0 * dot * nx
    rdy = dy - 2.0 * dot * ny
    rdz = dz - 2.0 * dot * nz
    # 递归
    return 1 + trace(hx, hy, hz, rdx, rdy, rdz, depth - 1)


def bench_trace():
    t1 = time.perf_counter()
    hits = 0
    for idx in range(NUM_RAYS):
        dx = (idx % 600 - 300) / 600.0
        dy = (idx % 1000 - 500) / 1000.0
        hits += trace(0.0, 0.0, 0.0, dx, dy, 1.0, 5)
    t2 = time.perf_counter()
    print(f"Phase C 递归trace: {NUM_RAYS}条射线(depth=5, {NS}球): {(t2 - t1) * 1000:.0f}ms (hits={hits})")


if __name__ == "__main__":
    init_scene()
    print("======== 光线追踪对比（标量优化版，无对象创建）========")
    print("")
    bench_index()
    bench_hit()
    bench_hit_single()
    bench_trace()
    print("")
    print("======== 完成 ========")
