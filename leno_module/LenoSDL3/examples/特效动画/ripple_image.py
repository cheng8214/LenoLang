#!/usr/bin/env python3
# ripple_image.py — 图片水波纹扭曲特效（Python 版性能基准）
#
# 对比 Leno 版本，测试同等算法下 Python 的帧率表现。
# 鼠标点击产生水波纹，自动随机产生波纹，ESC 退出。
#
# 运行：python ripple_image.py

import sys
import math
import random
import ctypes

# SDL3 路径（与 Leno 版共用同一个 DLL）
SDL3_DLL = r"d:\CLeno\LenoC\leno_module\LenoSDL3\lib\SDL3.dll"
SDL3_IMAGE_DLL = r"d:\CLeno\LenoC\leno_module\LenoSDL3\lib\SDL3_image.dll"

# ==================== SDL 常量 ====================
SDL_PIXELFORMAT_RGBA8888 = 373694468  # 和 Leno 版一致

# ==================== 加载 DLL ====================
sdl = ctypes.CDLL(SDL3_DLL)
sdl_img = ctypes.CDLL(SDL3_IMAGE_DLL)

# SDL_Init
sdl.SDL_Init.restype = ctypes.c_int
sdl.SDL_Init.argtypes = [ctypes.c_uint32]

# SDL_CreateWindow
sdl.SDL_CreateWindow.restype = ctypes.c_void_p
sdl.SDL_CreateWindow.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.c_int]

# SDL_CreateRenderer
sdl.SDL_CreateRenderer.restype = ctypes.c_void_p
sdl.SDL_CreateRenderer.argtypes = [ctypes.c_void_p, ctypes.c_char_p]

# SDL_CreateTexture
sdl.SDL_CreateTexture.restype = ctypes.c_void_p
sdl.SDL_CreateTexture.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int, ctypes.c_int, ctypes.c_int]

# SDL_UpdateTexture
sdl.SDL_UpdateTexture.restype = ctypes.c_int
sdl.SDL_UpdateTexture.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]

# SDL_RenderTexture
sdl.SDL_RenderTexture.restype = ctypes.c_int
sdl.SDL_RenderTexture.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]

# SDL_SetRenderDrawColor
sdl.SDL_SetRenderDrawColor.restype = ctypes.c_int
sdl.SDL_SetRenderDrawColor.argtypes = [ctypes.c_void_p, ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint8]

# SDL_RenderClear
sdl.SDL_RenderClear.restype = ctypes.c_int
sdl.SDL_RenderClear.argtypes = [ctypes.c_void_p]

# SDL_RenderPresent
sdl.SDL_RenderPresent.restype = None
sdl.SDL_RenderPresent.argtypes = [ctypes.c_void_p]

# SDL_PollEvent — SDL_Event 是 128 字节的 union，用 raw 字节数组即可
SDL_EVENT_SIZE = 128
class SDL_Event(ctypes.Structure):
    _fields_ = [("type", ctypes.c_uint32), ("data", ctypes.c_uint8 * 124)]

sdl.SDL_PollEvent.restype = ctypes.c_int
sdl.SDL_PollEvent.argtypes = [ctypes.POINTER(SDL_Event)]

# 事件类型常量
SDL_EVENT_QUIT = 0x100
SDL_EVENT_WINDOW_CLOSE_REQUESTED = 0x210
SDL_EVENT_KEY_DOWN = 0x300
SDL_EVENT_MOUSE_BUTTON_DOWN = 0x401
SDL_SCANCODE_ESCAPE = 41

# SDL_GetTicks
sdl.SDL_GetTicks.restype = ctypes.c_uint64
sdl.SDL_GetTicks.argtypes = []

# SDL_Delay
sdl.SDL_Delay.restype = None
sdl.SDL_Delay.argtypes = [ctypes.c_uint32]

# SDL_GetError
sdl.SDL_GetError.restype = ctypes.c_char_p
sdl.SDL_GetError.argtypes = []

# SDL_DestroyTexture / SDL_DestroyRenderer / SDL_DestroyWindow
sdl.SDL_DestroyTexture.restype = None
sdl.SDL_DestroyTexture.argtypes = [ctypes.c_void_p]
sdl.SDL_DestroyRenderer.restype = None
sdl.SDL_DestroyRenderer.argtypes = [ctypes.c_void_p]
sdl.SDL_DestroyWindow.restype = None
sdl.SDL_DestroyWindow.argtypes = [ctypes.c_void_p]
sdl.SDL_Quit.restype = None

# IMG_Load
sdl_img.IMG_Load.restype = ctypes.c_void_p
sdl_img.IMG_Load.argtypes = [ctypes.c_char_p]

# SDL_Surface 结构（前几个字段）
class SDL_Surface(ctypes.Structure):
    _fields_ = [
        ("flags", ctypes.c_uint32),
        ("format", ctypes.c_uint32),
        ("w", ctypes.c_int),
        ("h", ctypes.c_int),
        ("pitch", ctypes.c_int),
        ("pixels", ctypes.c_void_p),
        ("padding", ctypes.c_uint8 * 32),
    ]

# SDL_DestroySurface
sdl.SDL_DestroySurface.restype = None
sdl.SDL_DestroySurface.argtypes = [ctypes.c_void_p]

# SDL_SetRenderVSync
sdl.SDL_SetRenderVSync.restype = ctypes.c_int
sdl.SDL_SetRenderVSync.argtypes = [ctypes.c_void_p, ctypes.c_int]

# FRect for RenderTexture
class SDL_FRect(ctypes.Structure):
    _fields_ = [("x", ctypes.c_float), ("y", ctypes.c_float), ("w", ctypes.c_float), ("h", ctypes.c_float)]

# ==================== 配置 ====================
MAX_RIPPLES = 6
BAND_WIDTH = 16.0
MAX_RIP_W = 384

# ==================== Ripple ====================
class Ripple:
    __slots__ = ['x', 'y', 'radius', 'max_radius', 'speed', 'life', 'amplitude']
    def __init__(self, x, y):
        self.x = x
        self.y = y
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

# ==================== 像素格式转换 ====================
def load_and_convert(path):
    """加载图片，转换为 RGBA8888 bytearray，返回 (pixels, w, h)"""
    surf_ptr = sdl_img.IMG_Load(path.encode('utf-8'))
    if not surf_ptr:
        print(f"无法加载图片: {path}")
        print(f"SDL Error: {sdl.SDL_GetError().decode('utf-8', errors='replace')}")
        return None, 0, 0
    
    surf = ctypes.cast(surf_ptr, ctypes.POINTER(SDL_Surface)).contents
    orig_w = surf.w
    orig_h = surf.h
    surf_pitch = surf.pitch
    surf_format = surf.format
    
    print(f"图片已加载: {orig_w} x {orig_h}  format={surf_format}")
    
    # 降级
    scale = 1.0
    if orig_w > MAX_RIP_W:
        scale = MAX_RIP_W / orig_w
    
    buf_w = int(orig_w * scale)
    buf_h = int(orig_h * scale)
    if buf_w < 1: buf_w = 1
    if buf_h < 1: buf_h = 1
    
    print(f"处理分辨率: {buf_w} x {buf_h}  scale={int(scale*100)}%")
    
    # 获取像素指针
    raw_pixels = ctypes.cast(surf.pixels, ctypes.POINTER(ctypes.c_uint8 * (surf_pitch * orig_h))).contents
    
    # 判断 bpp
    bpp = 4
    if surf_format == 376840196:
        bpp = 4  # RGB888 (4字节)
    elif surf_format == 373694468:
        bpp = 4  # RGBA8888
    elif surf_format == 372645892:
        bpp = 4  # ARGB8888
    elif surf_pitch == orig_w * 3:
        bpp = 3  # 24bit RGB
    else:
        bpp = 4
    
    pix_size = buf_w * buf_h * 4
    src_pix = bytearray(pix_size)
    
    if scale == 1.0 and surf_format == 373694468:
        # RGBA8888 无降级：直接拷贝
        src_pix[:] = raw_pixels[:pix_size]
    else:
        # 通用路径：逐像素采样 + 格式转换
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
                        # RGB888: [B, G, R, X]
                        b = raw_pixels[si]
                        g = raw_pixels[si+1]
                        r = raw_pixels[si+2]
                        a = 255
                    elif surf_format == 372645892:
                        # ARGB8888: [B, G, R, A]
                        b = raw_pixels[si]
                        g = raw_pixels[si+1]
                        r = raw_pixels[si+2]
                        a = raw_pixels[si+3]
                    else:
                        # RGBA8888: [A, B, G, R]
                        a = raw_pixels[si]
                        b = raw_pixels[si+1]
                        g = raw_pixels[si+2]
                        r = raw_pixels[si+3]
                else:
                    # 24bit RGB: [R, G, B]
                    r = raw_pixels[si]
                    g = raw_pixels[si+1]
                    b = raw_pixels[si+2]
                    a = 255
                
                # RGBA8888 小端 u32 = (R<<24)|(G<<16)|(B<<8)|A, 内存 [A,B,G,R]
                src_pix[di] = a
                src_pix[di+1] = b
                src_pix[di+2] = g
                src_pix[di+3] = r
    
    sdl.SDL_DestroySurface(surf_ptr)
    return src_pix, buf_w, buf_h

# ==================== 渲染波纹 ====================
def render_ripple(src_pix, dst_pix, w, h, pitch):
    """渲染扭曲图像到 dst_pix"""
    # 先整块拷贝
    dst_pix[:] = src_pix
    
    if not ripples:
        return
    
    bw = BAND_WIDTH
    inv_bw = 1.0 / bw
    
    for rp in ripples:
        inner_r = rp.radius - bw
        outer_r = rp.radius + bw
        if inner_r < 0.0: inner_r = 0.0
        
        x0 = int(rp.x - outer_r)
        y0 = int(rp.y - outer_r)
        x1 = int(rp.x + outer_r) + 1
        y1 = int(rp.y + outer_r) + 1
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
                
                dist = math.sqrt(dist2)
                
                # diff 归一化到 [-1, 1]
                diff = (dist - rp.radius) * inv_bw
                if diff < -1.0: diff = -1.0
                elif diff > 1.0: diff = 1.0
                
                # 三角窗衰减
                tri = 1.0 - abs(diff)
                
                # 简化正弦
                wave = diff * (1.0 - diff * diff * 0.33) * 3.14
                amp = life_amp * tri * wave
                
                if dist > 0.5:
                    inv_dist = 1.0 / dist
                    sx = x + int(dx * inv_dist * amp)
                    sy = y + int(dy * inv_dist * amp)
                    if sx < 0: sx = 0
                    elif sx >= w: sx = w - 1
                    if sy < 0: sy = 0
                    elif sy >= h: sy = h - 1
                    
                    src_off = sy * pitch + sx * 4
                    dst_off = row_base + x * 4
                    dst_pix[dst_off:dst_off+4] = src_pix[src_off:src_off+4]

# ==================== 主函数 ====================
def main():
    sdl.SDL_Init(0x04)  # SDL_INIT_VIDEO = 4
    
    image_path = r"d:\sp.png"
    src_pix, buf_w, buf_h = load_and_convert(image_path)
    if src_pix is None:
        sdl.SDL_Quit()
        return
    
    buf_pitch = buf_w * 4
    pix_size = buf_w * buf_h * 4
    dst_pix = bytearray(pix_size)
    dst_pix[:] = src_pix
    
    win_w, win_h = 900, 650
    scale_x = win_w / buf_w
    scale_y = win_h / buf_h
    disp_scale = min(scale_x, scale_y)
    disp_img_w = buf_w * disp_scale
    disp_img_h = buf_h * disp_scale
    disp_x = (win_w - disp_img_w) * 0.5
    disp_y = (win_h - disp_img_h) * 0.5
    
    win = sdl.SDL_CreateWindow(
        b"Ripple Image Python - ESC to quit",
        win_w, win_h, 0x20  # SDL_WINDOW_RESIZABLE
    )
    # SDL3 签名: (title, w, h, flags)
    if not win:
        print("窗口创建失败")
        sdl.SDL_Quit()
        return
    
    ren = sdl.SDL_CreateRenderer(win, None)
    sdl.SDL_SetRenderVSync(ren, 0)  # 关闭 VSync 测上限
    
    tex = sdl.SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA8888, 1, buf_w, buf_h)
    if not tex:
        print("纹理创建失败")
        sdl.SDL_DestroyRenderer(ren)
        sdl.SDL_DestroyWindow(win)
        sdl.SDL_Quit()
        return
    
    # 用于 updateTexture 的 ctypes 缓冲
    src_ct = (ctypes.c_char * pix_size).from_buffer(src_pix)
    dst_ct = (ctypes.c_char * pix_size).from_buffer(dst_pix)
    
    event = SDL_Event()
    running = True
    last_auto = 0
    fps_count = 0
    cur_fps = 0
    last_fps_time = sdl.SDL_GetTicks()
    
    # ===== 性能统计 =====
    test_duration = 15000  # 15 秒
    perf_start = sdl.SDL_GetTicks()
    total_frames = 0
    min_frame_ms = 9999
    max_frame_ms = 0
    total_ms = 0
    
    # 初始波纹
    add_ripple(buf_w * 0.3, buf_h * 0.4)
    add_ripple(buf_w * 0.7, buf_h * 0.6)
    
    while running:
        t0 = sdl.SDL_GetTicks()
        
        # 超过测试时长自动退出
        if t0 - perf_start >= test_duration:
            running = False
        
        # 事件处理
        while sdl.SDL_PollEvent(ctypes.byref(event)):
            et = event.type
            if et == SDL_EVENT_QUIT or et == SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                running = False
            elif et == SDL_EVENT_KEY_DOWN:
                # SDL_KeyboardEvent: type(4) reserved(4) timestamp(8) windowID(4) which(4) scancode(4)
                # scancode 在偏移 24（event头算），data从偏移4开始，所以 data[20:24]
                scancode = int.from_bytes(bytes(event.data[20:24]), 'little')
                if scancode == SDL_SCANCODE_ESCAPE:
                    running = False
            elif et == SDL_EVENT_MOUSE_BUTTON_DOWN:
                # SDL_MouseButtonEvent: type(4) reserved(4) timestamp(8) windowID(4) which(4)
                # button(1@24) down(1@25) clicks(1@26) _pad(1@27) x(f32@28) y(f32@32)
                # data从偏移4开始，所以 button=data[20], x=data[24:28], y=data[28:32]
                d = event.data
                btn = d[20]
                if btn == 1:  # 左键
                    import struct
                    mx = struct.unpack_from('<f', bytes(d), 24)[0]
                    my = struct.unpack_from('<f', bytes(d), 28)[0]
                    bx = (mx - disp_x) / disp_scale
                    by = (my - disp_y) / disp_scale
                    if 0.0 <= bx < buf_w and 0.0 <= by < buf_h:
                        add_ripple(bx, by)
        
        # 自动产生波纹
        if t0 - last_auto > 2500:
            add_ripple(
                random.uniform(20.0, buf_w - 20.0),
                random.uniform(20.0, buf_h - 20.0)
            )
            last_auto = t0
        
        # 更新波纹
        i = len(ripples) - 1
        while i >= 0:
            rp = ripples[i]
            rp.radius += rp.speed
            rp.life = 1.0 - rp.radius / rp.max_radius
            if rp.life <= 0.0:
                ripples.pop(i)
            i -= 1
        
        # 渲染
        render_ripple(src_pix, dst_pix, buf_w, buf_h, buf_pitch)
        
        # 绘制
        sdl.SDL_SetRenderDrawColor(ren, 0x0A, 0x0F, 0x1E, 0xFF)
        sdl.SDL_RenderClear(ren)
        
        sdl.SDL_UpdateTexture(tex, None, dst_ct, buf_pitch)
        dst_rect = SDL_FRect(disp_x, disp_y, disp_img_w, disp_img_h)
        sdl.SDL_RenderTexture(ren, tex, None, ctypes.byref(dst_rect))
        
        sdl.SDL_RenderPresent(ren)
        
        # FPS 统计
        fps_count += 1
        if t0 - last_fps_time >= 1000:
            cur_fps = fps_count
            fps_count = 0
            last_fps_time = t0
            print(f"FPS: {cur_fps}  ripples: {len(ripples)}")
        
        elapsed = sdl.SDL_GetTicks() - t0
        total_ms += elapsed
        if elapsed < min_frame_ms: min_frame_ms = elapsed
        if elapsed > max_frame_ms: max_frame_ms = elapsed
        # 无帧延迟，跑满上限
        total_frames += 1
    
    # ===== 性能报告 =====
    actual_duration = sdl.SDL_GetTicks() - perf_start
    avg_fps = total_frames * 1000.0 / actual_duration if actual_duration > 0 else 0
    avg_frame_ms = total_ms / total_frames if total_frames > 0 else 0
    print("\n======================================")
    print("  Python 性能报告")
    print("======================================")
    print(f"运行时长: {actual_duration} ms")
    print(f"总帧数: {total_frames}")
    print(f"平均 FPS: {avg_fps:.1f}")
    print(f"平均每帧: {avg_frame_ms:.1f} ms")
    print(f"最低帧耗时: {min_frame_ms} ms")
    print(f"最高帧耗时: {max_frame_ms} ms")
    print(f"分辨率: {buf_w}x{buf_h} (原图 547x464)")
    print(f"波纹上限: {MAX_RIPPLES}  带宽: {BAND_WIDTH}")
    print("======================================")
    
    sdl.SDL_DestroyTexture(tex)
    sdl.SDL_DestroyRenderer(ren)
    sdl.SDL_DestroyWindow(win)
    sdl.SDL_Quit()
    print("退出成功")

if __name__ == "__main__":
    main()
