#!/usr/bin/env python3
# ripple_bench.py — Python 排除法性能定位测试
#
# 和 ripple_bench.leno 完全对应的开关，逐项禁用，对比 Python 各环节耗时。
# 运行：python ripple_bench.py

import sys
import math
import random
import ctypes

# ==================== 配置开关（和 Leno 版完全对应）====================
ENABLE_RENDER         = True   # 是否调用 render_ripple
ENABLE_SDL_DRAW       = True   # 是否调用 SDL 绘制
ENABLE_RIPPLE_FFI_COPY = True  # render_ripple 中是否做像素拷贝
ENABLE_RIPPLE_SQRT    = True   # render_ripple 中是否做 math.sqrt
ENABLE_EVENT_LOOP     = True   # 是否处理事件
ENABLE_AUTO_RIPPLE    = True   # 是否自动产生波纹
ENABLE_MEMCPY         = True   # render_ripple 开头是否 memcpy

# ==================== SDL 常量 ====================
SDL3_DLL = r"d:\CLeno\LenoC\leno_module\LenoSDL3\lib\SDL3.dll"
SDL3_IMAGE_DLL = r"d:\CLeno\LenoC\leno_module\LenoSDL3\lib\SDL3_image.dll"
SDL_PIXELFORMAT_RGBA8888 = 373694468

sdl = ctypes.CDLL(SDL3_DLL)
sdl_img = ctypes.CDLL(SDL3_IMAGE_DLL)

sdl.SDL_Init.restype = ctypes.c_int
sdl.SDL_Init.argtypes = [ctypes.c_uint32]
sdl.SDL_CreateWindow.restype = ctypes.c_void_p
sdl.SDL_CreateWindow.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.c_int]
sdl.SDL_CreateRenderer.restype = ctypes.c_void_p
sdl.SDL_CreateRenderer.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
sdl.SDL_CreateTexture.restype = ctypes.c_void_p
sdl.SDL_CreateTexture.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int, ctypes.c_int, ctypes.c_int]
sdl.SDL_UpdateTexture.restype = ctypes.c_int
sdl.SDL_UpdateTexture.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
sdl.SDL_RenderTexture.restype = ctypes.c_int
sdl.SDL_RenderTexture.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
sdl.SDL_SetRenderDrawColor.restype = ctypes.c_int
sdl.SDL_SetRenderDrawColor.argtypes = [ctypes.c_void_p, ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint8]
sdl.SDL_RenderClear.restype = ctypes.c_int
sdl.SDL_RenderClear.argtypes = [ctypes.c_void_p]
sdl.SDL_RenderPresent.restype = None
sdl.SDL_RenderPresent.argtypes = [ctypes.c_void_p]

SDL_EVENT_SIZE = 128
class SDL_Event(ctypes.Structure):
    _fields_ = [("type", ctypes.c_uint32), ("data", ctypes.c_uint8 * 124)]
sdl.SDL_PollEvent.restype = ctypes.c_int
sdl.SDL_PollEvent.argtypes = [ctypes.POINTER(SDL_Event)]

SDL_EVENT_QUIT = 0x100
SDL_EVENT_WINDOW_CLOSE_REQUESTED = 0x210
SDL_EVENT_KEY_DOWN = 0x300
SDL_EVENT_MOUSE_BUTTON_DOWN = 0x401
SDL_SCANCODE_ESCAPE = 41

sdl.SDL_GetTicks.restype = ctypes.c_uint64
sdl.SDL_GetTicks.argtypes = []
sdl.SDL_GetError.restype = ctypes.c_char_p
sdl.SDL_GetError.argtypes = []
sdl.SDL_DestroyTexture.restype = None
sdl.SDL_DestroyTexture.argtypes = [ctypes.c_void_p]
sdl.SDL_DestroyRenderer.restype = None
sdl.SDL_DestroyRenderer.argtypes = [ctypes.c_void_p]
sdl.SDL_DestroyWindow.restype = None
sdl.SDL_DestroyWindow.argtypes = [ctypes.c_void_p]
sdl.SDL_Quit.restype = None
sdl_img.IMG_Load.restype = ctypes.c_void_p
sdl_img.IMG_Load.argtypes = [ctypes.c_char_p]

class SDL_Surface(ctypes.Structure):
    _fields_ = [
        ("flags", ctypes.c_uint32), ("format", ctypes.c_uint32),
        ("w", ctypes.c_int), ("h", ctypes.c_int),
        ("pitch", ctypes.c_int), ("pixels", ctypes.c_void_p),
        ("padding", ctypes.c_uint8 * 32),
    ]
sdl.SDL_DestroySurface.restype = None
sdl.SDL_DestroySurface.argtypes = [ctypes.c_void_p]
sdl.SDL_SetRenderVSync.restype = ctypes.c_int
sdl.SDL_SetRenderVSync.argtypes = [ctypes.c_void_p, ctypes.c_int]

class SDL_FRect(ctypes.Structure):
    _fields_ = [("x", ctypes.c_float), ("y", ctypes.c_float), ("w", ctypes.c_float), ("h", ctypes.c_float)]

MAX_RIPPLES = 6
BAND_WIDTH = 16.0
MAX_RIP_W = 384

class Ripple:
    __slots__ = ['x', 'y', 'radius', 'max_radius', 'speed', 'life', 'amplitude']
    def __init__(self, x, y):
        self.x = x; self.y = y
        self.radius = 0.0
        self.max_radius = random.uniform(80.0, 150.0)
        self.speed = random.uniform(2.0, 3.0)
        self.life = 1.0
        self.amplitude = random.uniform(8.0, 16.0)

ripples = []

def add_ripple(x, y):
    if len(ripples) >= MAX_RIPPLES:
        ripples.pop(0)
    ripples.append(Ripple(x, y))

def load_and_convert(path):
    surf_ptr = sdl_img.IMG_Load(path.encode('utf-8'))
    if not surf_ptr: return None, 0, 0
    surf = ctypes.cast(surf_ptr, ctypes.POINTER(SDL_Surface)).contents
    orig_w, orig_h = surf.w, surf.h
    surf_pitch, surf_format = surf.pitch, surf.format
    raw_pixels = ctypes.cast(surf.pixels, ctypes.POINTER(ctypes.c_uint8 * (surf_pitch * orig_h))).contents
    scale = 1.0
    if orig_w > MAX_RIP_W: scale = MAX_RIP_W / orig_w
    buf_w, buf_h = int(orig_w * scale), int(orig_h * scale)
    if buf_w < 1: buf_w = 1
    if buf_h < 1: buf_h = 1
    bpp = 4
    if surf_pitch == orig_w * 3: bpp = 3
    pix_size = buf_w * buf_h * 4
    src_pix = bytearray(pix_size)
    if scale == 1.0 and surf_format == 373694468:
        src_pix[:] = raw_pixels[:pix_size]
    else:
        inv_scale = 1.0 / scale if scale > 0 else 1.0
        for dy in range(buf_h):
            src_y = int(dy * inv_scale)
            if src_y >= orig_h: src_y = orig_h - 1
            src_row = src_y * surf_pitch
            dst_row = dy * buf_w * 4
            for dx in range(buf_w):
                src_x = int(dx * inv_scale)
                if src_x >= orig_w: src_x = orig_w - 1
                si = src_row + src_x * bpp
                di = dst_row + dx * 4
                if bpp == 4:
                    if surf_format == 376840196:
                        b, g, r, a = raw_pixels[si], raw_pixels[si+1], raw_pixels[si+2], 255
                    elif surf_format == 372645892:
                        b, g, r, a = raw_pixels[si], raw_pixels[si+1], raw_pixels[si+2], raw_pixels[si+3]
                    else:
                        a, b, g, r = raw_pixels[si], raw_pixels[si+1], raw_pixels[si+2], raw_pixels[si+3]
                else:
                    r, g, b, a = raw_pixels[si], raw_pixels[si+1], raw_pixels[si+2], 255
                src_pix[di] = a; src_pix[di+1] = b; src_pix[di+2] = g; src_pix[di+3] = r
    sdl.SDL_DestroySurface(surf_ptr)
    return src_pix, buf_w, buf_h

# ==================== 渲染波纹（可逐项禁用）====================
def render_ripple(src_pix, dst_pix, w, h, pitch):
    if ENABLE_MEMCPY:
        dst_pix[:] = src_pix
    if not ripples:
        return
    bw = BAND_WIDTH
    inv_bw = 1.0 / bw
    for rp in ripples:
        inner_r = rp.radius - bw
        outer_r = rp.radius + bw
        if inner_r < 0.0: inner_r = 0.0
        x0 = int(rp.x - outer_r); y0 = int(rp.y - outer_r)
        x1 = int(rp.x + outer_r) + 1; y1 = int(rp.y + outer_r) + 1
        if x0 < 0: x0 = 0
        if y0 < 0: y0 = 0
        if x1 > w: x1 = w
        if y1 > h: y1 = h
        outer_r2 = outer_r * outer_r
        inner_r2 = inner_r * inner_r
        life_amp = rp.amplitude * rp.life
        for y in range(y0, y1):
            fy = float(y)
            dy = fy - rp.y
            dy2 = dy * dy
            row_base = y * pitch
            for x in range(x0, x1):
                fx = float(x)
                dx = fx - rp.x
                dist2 = dx * dx + dy2
                if dist2 > outer_r2 or dist2 < inner_r2:
                    continue
                dist = dist2
                inv_dist = 1.0
                if ENABLE_RIPPLE_SQRT:
                    dist = math.sqrt(dist2)
                    inv_dist = 1.0 / dist
                diff = (dist - rp.radius) * inv_bw
                if diff < -1.0: diff = -1.0
                elif diff > 1.0: diff = 1.0
                tri = 1.0 - abs(diff)
                wave = diff * (1.0 - diff * diff * 0.33) * 3.14
                amp = life_amp * tri * wave
                if dist > 0.5:
                    sx = x + int(dx * inv_dist * amp)
                    sy = y + int(dy * inv_dist * amp)
                    if sx < 0: sx = 0
                    elif sx >= w: sx = w - 1
                    if sy < 0: sy = 0
                    elif sy >= h: sy = h - 1
                    if ENABLE_RIPPLE_FFI_COPY:
                        src_off = sy * pitch + sx * 4
                        dst_off = row_base + x * 4
                        dst_pix[dst_off:dst_off+4] = src_pix[src_off:src_off+4]

def main():
    sdl.SDL_Init(0x04)
    src_pix, buf_w, buf_h = load_and_convert(r"d:\sp.png")
    if src_pix is None:
        sdl.SDL_Quit(); return
    buf_pitch = buf_w * 4
    pix_size = buf_w * buf_h * 4
    dst_pix = bytearray(pix_size)
    dst_pix[:] = src_pix
    win_w, win_h = 900, 650
    scale_x = win_w / buf_w; scale_y = win_h / buf_h
    disp_scale = min(scale_x, scale_y)
    disp_img_w = buf_w * disp_scale; disp_img_h = buf_h * disp_scale
    disp_x = (win_w - disp_img_w) * 0.5; disp_y = (win_h - disp_img_h) * 0.5
    win = sdl.SDL_CreateWindow(b"Ripple Bench Python", win_w, win_h, 0x20)
    ren = sdl.SDL_CreateRenderer(win, None)
    sdl.SDL_SetRenderVSync(ren, 0)
    tex = sdl.SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA8888, 1, buf_w, buf_h)
    dst_ct = (ctypes.c_char * pix_size).from_buffer(dst_pix)
    event = SDL_Event()
    running = True
    last_auto = 0
    test_duration = 15000
    perf_start = sdl.SDL_GetTicks()
    total_frames = 0; total_ms = 0; min_frame_ms = 9999; max_frame_ms = 0

    add_ripple(buf_w * 0.3, buf_h * 0.4)
    add_ripple(buf_w * 0.7, buf_h * 0.6)

    print("===== 排除法配置 =====")
    print(f"ENABLE_RENDER={ENABLE_RENDER} ENABLE_SDL_DRAW={ENABLE_SDL_DRAW}")
    print(f"ENABLE_RIPPLE_FFI_COPY={ENABLE_RIPPLE_FFI_COPY} ENABLE_RIPPLE_SQRT={ENABLE_RIPPLE_SQRT}")
    print(f"ENABLE_EVENT_LOOP={ENABLE_EVENT_LOOP} ENABLE_AUTO_RIPPLE={ENABLE_AUTO_RIPPLE}")
    print(f"ENABLE_MEMCPY={ENABLE_MEMCPY}")
    print("======================")

    while running:
        t0 = sdl.SDL_GetTicks()
        if t0 - perf_start >= test_duration:
            running = False
        if ENABLE_EVENT_LOOP:
            while sdl.SDL_PollEvent(ctypes.byref(event)):
                et = event.type
                if et == SDL_EVENT_QUIT or et == SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                    running = False
                elif et == SDL_EVENT_KEY_DOWN:
                    scancode = int.from_bytes(bytes(event.data[20:24]), 'little')
                    if scancode == SDL_SCANCODE_ESCAPE:
                        running = False
                elif et == SDL_EVENT_MOUSE_BUTTON_DOWN:
                    d = event.data
                    if d[20] == 1:
                        import struct
                        mx = struct.unpack_from('<f', bytes(d), 24)[0]
                        my = struct.unpack_from('<f', bytes(d), 28)[0]
                        bx = (mx - disp_x) / disp_scale
                        by = (my - disp_y) / disp_scale
                        if 0.0 <= bx < buf_w and 0.0 <= by < buf_h:
                            add_ripple(bx, by)
        if ENABLE_AUTO_RIPPLE:
            if t0 - last_auto > 2500:
                add_ripple(random.uniform(20.0, buf_w - 20.0), random.uniform(20.0, buf_h - 20.0))
                last_auto = t0
        i = len(ripples) - 1
        while i >= 0:
            rp = ripples[i]
            rp.radius += rp.speed
            rp.life = 1.0 - rp.radius / rp.max_radius
            if rp.life <= 0.0:
                ripples.pop(i)
            i -= 1
        if ENABLE_RENDER:
            render_ripple(src_pix, dst_pix, buf_w, buf_h, buf_pitch)
        if ENABLE_SDL_DRAW:
            sdl.SDL_SetRenderDrawColor(ren, 0x0A, 0x0F, 0x1E, 0xFF)
            sdl.SDL_RenderClear(ren)
            sdl.SDL_UpdateTexture(tex, None, dst_ct, buf_pitch)
            dst_rect = SDL_FRect(disp_x, disp_y, disp_img_w, disp_img_h)
            sdl.SDL_RenderTexture(ren, tex, None, ctypes.byref(dst_rect))
            sdl.SDL_RenderPresent(ren)
        elapsed = sdl.SDL_GetTicks() - t0
        total_ms += elapsed
        if elapsed < min_frame_ms: min_frame_ms = elapsed
        if elapsed > max_frame_ms: max_frame_ms = elapsed
        total_frames += 1

    actual_duration = sdl.SDL_GetTicks() - perf_start
    avg_fps = total_frames * 1000.0 / actual_duration if actual_duration > 0 else 0
    avg_frame_ms = total_ms / total_frames if total_frames > 0 else 0
    print("\n===== 性能结果 =====")
    print(f"运行: {actual_duration} ms  帧数: {total_frames}")
    print(f"平均 FPS: {avg_fps:.1f}  每帧: {avg_frame_ms:.4f} ms")
    print(f"最低: {min_frame_ms} ms  最高: {max_frame_ms} ms")
    print("====================")
    sdl.SDL_DestroyTexture(tex)
    sdl.SDL_DestroyRenderer(ren)
    sdl.SDL_DestroyWindow(win)
    sdl.SDL_Quit()

if __name__ == "__main__":
    main()
