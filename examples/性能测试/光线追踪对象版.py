import time
import math

# 光线追踪对比测试（对象版：class/struct + 方法 + Hit 对象分配）
# 与 Leno struct 版一一对应：
#   Phase A: 对象数组索引 + 字段访问
#   Phase B: Sphere.hit() 方法调用，返回 Hit 对象
#   Phase B2: Sphere.hit_single() 单返回值方法
#   Phase C: 完整递归 trace（每球测试分配一个 Hit 对象）


class Hit:
    __slots__ = ("t", "hit")

    def __init__(self, t=-1.0, hit=0):
        self.t = t
        self.hit = hit


class Sphere:
    __slots__ = ("cx", "cy", "cz", "r")

    def __init__(self, cx, cy, cz, r):
        self.cx = cx
        self.cy = cy
        self.cz = cz
        self.r = r

    def hit(self, ox, oy, oz, dx, dy, dz):
        ocx = ox - self.cx
        ocy = oy - self.cy
        ocz = oz - self.cz
        r = self.r
        b = ocx * dx + ocy * dy + ocz * dz
        c = ocx * ocx + ocy * ocy + ocz * ocz - r * r
        disc = b * b - c
        if disc < 0.0:
            return Hit(-1.0, 0)
        sq = math.sqrt(disc)
        t = -b - sq
        if t > 0.001:
            return Hit(t, 1)
        t = -b + sq
        if t > 0.001:
            return Hit(t, 1)
        return Hit(-1.0, 0)

    def hit_single(self, ox, oy, oz, dx, dy, dz):
        ocx = ox - self.cx
        ocy = oy - self.cy
        ocz = oz - self.cz
        r = self.r
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


NS = 10
NUM_RAYS = 300000
HITCALLS = 3000000

spheres = []


def init_scene():
    for i in range(NS):
        spheres.append(Sphere(i * 2.0 - 9.0,
                              ((i * 37) % 11 - 5) * 0.7,
                              12.0 + (i % 3) * 4.0,
                              1.0 + (i % 3) * 0.5))


# ==================== Phase A: 对象数组索引 + 字段访问 ====================
def bench_index():
    t1 = time.perf_counter()
    sum_val = 0.0
    si = 0
    for _ in range(HITCALLS):
        s = spheres[si]
        sum_val += s.cx + s.cy + s.cz + s.r
        si += 1
        if si >= NS:
            si = 0
    t2 = time.perf_counter()
    print(f"Phase A 对象字段访问: {HITCALLS}次循环(1次索引+4次字段/轮): {(t2 - t1) * 1000:.0f}ms (sum={sum_val})")


# ==================== Phase B: Sphere.hit() 方法调用（返回 Hit 对象） ====================
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
        h = spheres[si].hit(ox, oy, oz, dx, dy, dz)
        hits += h.hit
        si += 1
        if si >= NS:
            si = 0
    t2 = time.perf_counter()
    print(f"Phase B hit方法+Hit对象: {HITCALLS}次调用: {(t2 - t1) * 1000:.0f}ms (hits={hits})")


# ==================== Phase B2: Sphere.hit_single() 单返回值方法 ====================
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
        t = spheres[si].hit_single(ox, oy, oz, dx, dy, dz)
        if t > 0.0:
            hits += 1
        si += 1
        if si >= NS:
            si = 0
    t2 = time.perf_counter()
    print(f"Phase B2 hit_single方法: {HITCALLS}次调用: {(t2 - t1) * 1000:.0f}ms (hits={hits})")


# ==================== Phase C: 递归 trace（对象版） ====================
def trace(ox, oy, oz, dx, dy, dz, depth):
    if depth <= 0:
        return 0
    tmin = 1000000000.0
    hit_idx = -1
    for si in range(NS):
        h = spheres[si].hit(ox, oy, oz, dx, dy, dz)
        if h.hit == 1:
            if h.t < tmin:
                tmin = h.t
                hit_idx = si
    if hit_idx < 0:
        return 0
    # 命中点
    hx = ox + dx * tmin
    hy = oy + dy * tmin
    hz = oz + dz * tmin
    # 法线（除以半径）
    s = spheres[hit_idx]
    r = s.r
    nx = (hx - s.cx) / r
    ny = (hy - s.cy) / r
    nz = (hz - s.cz) / r
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
    print(f"Phase C 递归trace(对象版): {NUM_RAYS}条射线(depth=5, {NS}球): {(t2 - t1) * 1000:.0f}ms (hits={hits})")


if __name__ == "__main__":
    init_scene()
    print("======== 光线追踪对比（对象版：class/struct + 方法 + Hit 对象）========")
    print("")
    bench_index()
    bench_hit()
    bench_hit_single()
    bench_trace()
    print("")
    print("======== 完成 ========")
