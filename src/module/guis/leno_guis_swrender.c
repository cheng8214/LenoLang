/* Leno GUI - 软件渲染共享代码
 * 三个平台（Win32/Linux/macOS）共用的像素缓冲区渲染逻辑
 * 通过 #include 方式引入，可直接访问平台文件中定义的结构体
 *
 * 包含: 软件渲染辅助函数、绘图 API、视口裁剪、纹理操作、文字渲染
 */

/* ===== 软件渲染辅助函数 ===== */

static void sw_draw_point(LenoGUIPlatformRenderer* ren, int x, int y, uint32_t color) {
    int px = x + ren->vp_x;
    int py = y + ren->vp_y;
    if (px < ren->vp_x || px >= ren->vp_x + ren->vp_w) return;
    if (py < ren->vp_y || py >= ren->vp_y + ren->vp_h) return;
    if (ren->clip_enabled) {
        if (px < ren->clip_x || px >= ren->clip_x + ren->clip_w) return;
        if (py < ren->clip_y || py >= ren->clip_y + ren->clip_h) return;
    }
    if (px >= 0 && px < ren->width && py >= 0 && py < ren->height) {
        ren->pixels[py * ren->width + px] = color;
    }
}

static void sw_draw_line(LenoGUIPlatformRenderer* ren, int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = abs(x1 - x0);
    int dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        sw_draw_point(ren, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void sw_draw_rect(LenoGUIPlatformRenderer* ren, int x, int y, int w, int h, uint32_t color) {
    for (int i = 0; i < w; i++) {
        sw_draw_point(ren, x + i, y, color);
        sw_draw_point(ren, x + i, y + h - 1, color);
    }
    for (int i = 0; i < h; i++) {
        sw_draw_point(ren, x, y + i, color);
        sw_draw_point(ren, x + w - 1, y + i, color);
    }
}

static void sw_fill_rect(LenoGUIPlatformRenderer* ren, int x, int y, int w, int h, uint32_t color) {
    int px = x + ren->vp_x;
    int py = y + ren->vp_y;
    int x1 = px > ren->vp_x ? px : ren->vp_x;
    int y1 = py > ren->vp_y ? py : ren->vp_y;
    int x2 = (px + w) < (ren->vp_x + ren->vp_w) ? (px + w) : (ren->vp_x + ren->vp_w);
    int y2 = (py + h) < (ren->vp_y + ren->vp_h) ? (py + h) : (ren->vp_y + ren->vp_h);
    if (ren->clip_enabled) {
        if (x1 < ren->clip_x) x1 = ren->clip_x;
        if (y1 < ren->clip_y) y1 = ren->clip_y;
        if (x2 > ren->clip_x + ren->clip_w) x2 = ren->clip_x + ren->clip_w;
        if (y2 > ren->clip_y + ren->clip_h) y2 = ren->clip_y + ren->clip_h;
    }
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > ren->width) x2 = ren->width;
    if (y2 > ren->height) y2 = ren->height;
    if (x1 >= x2 || y1 >= y2) return;
    for (int row = y1; row < y2; row++) {
        uint32_t* row_ptr = ren->pixels + row * ren->width;
        for (int col = x1; col < x2; col++) {
            row_ptr[col] = color;
        }
    }
}

static void sw_blit_texture(LenoGUIPlatformRenderer* ren,
                             const uint32_t* src, int sw_val, int sh_val, int spitch,
                             int dx, int dy) {
    for (int y = 0; y < sh_val; y++) {
        int py = dy + y + ren->vp_y;
        if (py < ren->vp_y || py >= ren->vp_y + ren->vp_h) continue;
        if (ren->clip_enabled && (py < ren->clip_y || py >= ren->clip_y + ren->clip_h)) continue;
        if (py < 0 || py >= ren->height) continue;
        for (int x = 0; x < sw_val; x++) {
            int px = dx + x + ren->vp_x;
            if (px < ren->vp_x || px >= ren->vp_x + ren->vp_w) continue;
            if (ren->clip_enabled && (px < ren->clip_x || px >= ren->clip_x + ren->clip_w)) continue;
            if (px < 0 || px >= ren->width) continue;
            uint32_t src_pixel = src[y * (spitch / 4) + x];
            uint8_t sa = (src_pixel >> 24) & 0xFF;
            if (sa == 0) continue;
            if (sa == 255) {
                ren->pixels[py * ren->width + px] = src_pixel;
            } else {
                uint32_t dst_pixel = ren->pixels[py * ren->width + px];
                uint8_t dr = (dst_pixel >> 16) & 0xFF;
                uint8_t dg = (dst_pixel >> 8) & 0xFF;
                uint8_t db = dst_pixel & 0xFF;
                uint8_t sr = (src_pixel >> 16) & 0xFF;
                uint8_t sg = (src_pixel >> 8) & 0xFF;
                uint8_t sb = src_pixel & 0xFF;
                uint8_t inv_a = 255 - sa;
                dr = (uint8_t)((sr * sa + dr * inv_a) / 255);
                dg = (uint8_t)((sg * sa + dg * inv_a) / 255);
                db = (uint8_t)((sb * sa + db * inv_a) / 255);
                ren->pixels[py * ren->width + px] = LENO_GUI_PIXEL(dr, dg, db, 255);
            }
        }
    }
}

/* ===== 绘图 API（平台无关，操作像素缓冲区） ===== */

void leno_gui_platform_render_clear(LenoGUIPlatformRenderer* ren) {
    if (!ren || !ren->pixels) return;
    uint32_t color = LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a);
    int total = ren->width * ren->height;
    for (int i = 0; i < total; i++) {
        ren->pixels[i] = color;
    }
}

void leno_gui_platform_set_draw_color(LenoGUIPlatformRenderer* ren, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (!ren) return;
    ren->draw_r = r;
    ren->draw_g = g;
    ren->draw_b = b;
    ren->draw_a = a;
}

void leno_gui_platform_render_draw_point(LenoGUIPlatformRenderer* ren, int x, int y) {
    if (!ren || !ren->pixels) return;
    uint32_t color = LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a);
    sw_draw_point(ren, x, y, color);
}

void leno_gui_platform_render_draw_line(LenoGUIPlatformRenderer* ren, int x1, int y1, int x2, int y2) {
    if (!ren || !ren->pixels) return;
    uint32_t color = LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a);
    sw_draw_line(ren, x1, y1, x2, y2, color);
}

void leno_gui_platform_render_draw_rect(LenoGUIPlatformRenderer* ren, int x, int y, int w, int h) {
    if (!ren || !ren->pixels) return;
    uint32_t color = LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a);
    sw_draw_rect(ren, x, y, w, h, color);
}

void leno_gui_platform_render_fill_rect(LenoGUIPlatformRenderer* ren, int x, int y, int w, int h) {
    if (!ren || !ren->pixels) return;
    uint32_t color = LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a);
    sw_fill_rect(ren, x, y, w, h, color);
}

void leno_gui_platform_get_renderer_size(LenoGUIPlatformRenderer* ren, int* w, int* h) {
    if (!ren) { if (w) *w = 0; if (h) *h = 0; return; }
    if (w) *w = ren->width;
    if (h) *h = ren->height;
}

void leno_gui_platform_renderer_mark_resize(LenoGUIPlatformRenderer* ren) {
    if (ren) ren->needs_resize = 1;
}

/* ===== 画圆（Bresenham 中点圆算法） ===== */

void leno_gui_platform_render_draw_circle(LenoGUIPlatformRenderer* ren, int cx, int cy, int radius) {
    if (!ren || !ren->pixels || radius <= 0) return;
    uint32_t color = LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a);
    int x = 0, y = radius;
    int d = 3 - 2 * radius;
    while (x <= y) {
        sw_draw_point(ren, cx + x, cy + y, color);
        sw_draw_point(ren, cx - x, cy + y, color);
        sw_draw_point(ren, cx + x, cy - y, color);
        sw_draw_point(ren, cx - x, cy - y, color);
        sw_draw_point(ren, cx + y, cy + x, color);
        sw_draw_point(ren, cx - y, cy + x, color);
        sw_draw_point(ren, cx + y, cy - x, color);
        sw_draw_point(ren, cx - y, cy - x, color);
        if (d < 0) {
            d += 4 * x + 6;
        } else {
            d += 4 * (x - y) + 10;
            y--;
        }
        x++;
    }
}

void leno_gui_platform_render_fill_circle(LenoGUIPlatformRenderer* ren, int cx, int cy, int radius) {
    if (!ren || !ren->pixels || radius <= 0) return;
    uint32_t color = LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a);
    int x = 0, y = radius;
    int d = 3 - 2 * radius;
    while (x <= y) {
        sw_draw_line(ren, cx - x, cy + y, cx + x, cy + y, color);
        sw_draw_line(ren, cx - x, cy - y, cx + x, cy - y, color);
        sw_draw_line(ren, cx - y, cy + x, cx + y, cy + x, color);
        sw_draw_line(ren, cx - y, cy - x, cx + y, cy - x, color);
        if (d < 0) {
            d += 4 * x + 6;
        } else {
            d += 4 * (x - y) + 10;
            y--;
        }
        x++;
    }
}

/* ===== 圆角矩形 ===== */

void leno_gui_platform_render_draw_rounded_rect(LenoGUIPlatformRenderer* ren, int x, int y, int w, int h, int radius) {
    if (!ren || !ren->pixels || w <= 0 || h <= 0) return;
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;
    if (radius <= 0) {
        leno_gui_platform_render_draw_rect(ren, x, y, w, h);
        return;
    }
    uint32_t color = LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a);
    sw_draw_line(ren, x + radius, y, x + w - 1 - radius, y, color);
    sw_draw_line(ren, x + radius, y + h - 1, x + w - 1 - radius, y + h - 1, color);
    sw_draw_line(ren, x, y + radius, x, y + h - 1 - radius, color);
    sw_draw_line(ren, x + w - 1, y + radius, x + w - 1, y + h - 1 - radius, color);
    int cx1 = x + radius, cy1 = y + radius;
    int cx2 = x + w - 1 - radius, cy2 = y + radius;
    int cx3 = x + radius, cy3 = y + h - 1 - radius;
    int cx4 = x + w - 1 - radius, cy4 = y + h - 1 - radius;
    int ax = 0, ay = radius;
    int dd = 3 - 2 * radius;
    while (ax <= ay) {
        sw_draw_point(ren, cx1 - ax, cy1 - ay, color);
        sw_draw_point(ren, cx1 - ay, cy1 - ax, color);
        sw_draw_point(ren, cx2 + ax, cy2 - ay, color);
        sw_draw_point(ren, cx2 + ay, cy2 - ax, color);
        sw_draw_point(ren, cx3 - ax, cy3 + ay, color);
        sw_draw_point(ren, cx3 - ay, cy3 + ax, color);
        sw_draw_point(ren, cx4 + ax, cy4 + ay, color);
        sw_draw_point(ren, cx4 + ay, cy4 + ax, color);
        if (dd < 0) {
            dd += 4 * ax + 6;
        } else {
            dd += 4 * (ax - ay) + 10;
            ay--;
        }
        ax++;
    }
}

void leno_gui_platform_render_fill_rounded_rect(LenoGUIPlatformRenderer* ren, int x, int y, int w, int h, int radius) {
    if (!ren || !ren->pixels || w <= 0 || h <= 0) return;
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;
    if (radius <= 0) {
        leno_gui_platform_render_fill_rect(ren, x, y, w, h);
        return;
    }
    leno_gui_platform_render_fill_rect(ren, x, y + radius, w, h - 2 * radius);
    leno_gui_platform_render_fill_rect(ren, x + radius, y, w - 2 * radius, radius);
    leno_gui_platform_render_fill_rect(ren, x + radius, y + h - radius, w - 2 * radius, radius);
    int ax = 0, ay = radius;
    int dd = 3 - 2 * radius;
    while (ax <= ay) {
        sw_draw_line(ren, x + radius - ay, y + radius - ax, x + radius - 1, y + radius - ax, LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a));
        sw_draw_line(ren, x + radius - ax, y + radius - ay, x + radius - 1, y + radius - ay, LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a));
        sw_draw_line(ren, x + w - radius, y + radius - ax, x + w - radius + ay - 1, y + radius - ax, LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a));
        sw_draw_line(ren, x + w - radius, y + radius - ay, x + w - radius + ax - 1, y + radius - ay, LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a));
        sw_draw_line(ren, x + radius - ay, y + h - radius + ax, x + radius - 1, y + h - radius + ax, LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a));
        sw_draw_line(ren, x + radius - ax, y + h - radius + ay, x + radius - 1, y + h - radius + ay, LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a));
        sw_draw_line(ren, x + w - radius, y + h - radius + ax, x + w - radius + ay - 1, y + h - radius + ax, LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a));
        sw_draw_line(ren, x + w - radius, y + h - radius + ay, x + w - radius + ax - 1, y + h - radius + ay, LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a));
        if (dd < 0) {
            dd += 4 * ax + 6;
        } else {
            dd += 4 * (ax - ay) + 10;
            ay--;
        }
        ax++;
    }
}

/* ===== 视口和裁剪（参考 SDL3） ===== */

void leno_gui_platform_set_viewport(LenoGUIPlatformRenderer* ren, int x, int y, int w, int h) {
    if (!ren) return;
    ren->vp_x = x;
    ren->vp_y = y;
    ren->vp_w = w > 0 ? w : ren->width;
    ren->vp_h = h > 0 ? h : ren->height;
}

void leno_gui_platform_get_viewport(LenoGUIPlatformRenderer* ren, int* x, int* y, int* w, int* h) {
    if (!ren) { if (x) *x = 0; if (y) *y = 0; if (w) *w = 0; if (h) *h = 0; return; }
    if (x) *x = ren->vp_x;
    if (y) *y = ren->vp_y;
    if (w) *w = ren->vp_w;
    if (h) *h = ren->vp_h;
}

void leno_gui_platform_set_clip_rect(LenoGUIPlatformRenderer* ren, int x, int y, int w, int h) {
    if (!ren) return;
    ren->clip_x = x;
    ren->clip_y = y;
    ren->clip_w = w;
    ren->clip_h = h;
    ren->clip_enabled = 1;
}

void leno_gui_platform_get_clip_rect(LenoGUIPlatformRenderer* ren, int* x, int* y, int* w, int* h) {
    if (!ren) { if (x) *x = 0; if (y) *y = 0; if (w) *w = 0; if (h) *h = 0; return; }
    if (x) *x = ren->clip_x;
    if (y) *y = ren->clip_y;
    if (w) *w = ren->clip_w;
    if (h) *h = ren->clip_h;
}

void leno_gui_platform_disable_clip_rect(LenoGUIPlatformRenderer* ren) {
    if (!ren) return;
    ren->clip_enabled = 0;
}

/* ===== 纹理操作 ===== */

LenoGUIPlatformTexture* leno_gui_platform_create_texture(LenoGUIPlatformRenderer* ren, int w, int h) {
    (void)ren;
    LenoGUIPlatformTexture* tex = (LenoGUIPlatformTexture*)calloc(1, sizeof(LenoGUIPlatformTexture));
    if (!tex) return NULL;
    tex->width = w;
    tex->height = h;
    tex->pitch = w * 4;
    tex->pixels = (uint32_t*)calloc(w * h, sizeof(uint32_t));
    if (!tex->pixels) {
        free(tex);
        return NULL;
    }
    return tex;
}

void leno_gui_platform_destroy_texture(LenoGUIPlatformTexture* tex) {
    if (!tex) return;
    if (tex->pixels) free(tex->pixels);
    free(tex);
}

void leno_gui_platform_render_texture(LenoGUIPlatformRenderer* ren, LenoGUIPlatformTexture* tex, int x, int y) {
    if (!ren || !ren->pixels || !tex || !tex->pixels) return;
    sw_blit_texture(ren, tex->pixels, tex->width, tex->height, tex->pitch, x, y);
}

void leno_gui_platform_update_texture(LenoGUIPlatformTexture* tex, const void* data, int pitch) {
    if (!tex || !tex->pixels || !data) return;
    const uint32_t* src = (const uint32_t*)data;
    for (int y = 0; y < tex->height; y++) {
        memcpy(tex->pixels + y * tex->width, (const uint8_t*)src + y * pitch, tex->width * 4);
    }
}

int leno_gui_platform_texture_width(LenoGUIPlatformTexture* tex) {
    return tex ? tex->width : 0;
}

int leno_gui_platform_texture_height(LenoGUIPlatformTexture* tex) {
    return tex ? tex->height : 0;
}

void leno_gui_platform_render_texture_src(LenoGUIPlatformRenderer* ren, LenoGUIPlatformTexture* tex,
                                          int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh) {
    if (!ren || !ren->pixels || !tex || !tex->pixels) return;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    float x_scale = (float)sw / (float)dw;
    float y_scale = (float)sh / (float)dh;
    int src_pitch_int = tex->pitch / 4;
    for (int row = 0; row < dh; row++) {
        int src_y = sy + (int)(row * y_scale);
        if (src_y < 0 || src_y >= tex->height) continue;
        for (int col = 0; col < dw; col++) {
            int src_x = sx + (int)(col * x_scale);
            if (src_x < 0 || src_x >= tex->width) continue;
            uint32_t src_pixel = tex->pixels[src_y * src_pitch_int + src_x];
            uint8_t sa = (src_pixel >> 24) & 0xFF;
            if (sa == 0) continue;
            if (sa == 255) {
                int px = dx + col + ren->vp_x;
                int py = dy + row + ren->vp_y;
                if (px >= ren->vp_x && px < ren->vp_x + ren->vp_w &&
                    py >= ren->vp_y && py < ren->vp_y + ren->vp_h) {
                    if (!ren->clip_enabled || (px >= ren->clip_x && px < ren->clip_x + ren->clip_w &&
                                                py >= ren->clip_y && py < ren->clip_y + ren->clip_h)) {
                        if (px >= 0 && px < ren->width && py >= 0 && py < ren->height) {
                            ren->pixels[py * ren->width + px] = src_pixel;
                        }
                    }
                }
            } else {
                sw_draw_point(ren, dx + col, dy + row, src_pixel);
            }
        }
    }
}

void leno_gui_platform_render_texture_rotated(LenoGUIPlatformRenderer* ren, LenoGUIPlatformTexture* tex,
                                               int x, int y, double angle, int flip) {
    if (!ren || !ren->pixels || !tex || !tex->pixels) return;
    double rad = angle * 3.14159265358979323846 / 180.0;
    double cos_a = cos(rad);
    double sin_a = sin(rad);
    int tw = tex->width;
    int th = tex->height;
    double cx = tw / 2.0;
    double cy = th / 2.0;
    int src_pitch_int = tex->pitch / 4;
    for (int dy = -th; dy <= th; dy++) {
        for (int dx = -tw; dx <= tw; dx++) {
            double fx = dx, fy = dy;
            if (flip & LENO_GUI_FLIP_HORIZONTAL) fx = -fx;
            if (flip & LENO_GUI_FLIP_VERTICAL) fy = -fy;
            double src_x = cos_a * fx + sin_a * fy + cx;
            double src_y = -sin_a * fx + cos_a * fy + cy;
            int isx = (int)(src_x + 0.5);
            int isy = (int)(src_y + 0.5);
            if (isx < 0 || isx >= tw || isy < 0 || isy >= th) continue;
            uint32_t src_pixel = tex->pixels[isy * src_pitch_int + isx];
            uint8_t sa = (src_pixel >> 24) & 0xFF;
            if (sa == 0) continue;
            sw_draw_point(ren, x + dx, y + dy, src_pixel);
        }
    }
}

/* ===== 文字渲染（内置 8x8 点阵字体，参考 SDL3 SDL_RenderDebugText） ===== */

static const uint8_t g_font8x8[96][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00},
    {0x6c,0x6c,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x6c,0x6c,0xfe,0x6c,0xfe,0x6c,0x6c,0x00},
    {0x18,0x3e,0x60,0x3c,0x06,0x7c,0x18,0x00},
    {0x00,0xc6,0xcc,0x18,0x30,0x66,0xc6,0x00},
    {0x38,0x6c,0x38,0x76,0xdc,0xcc,0x76,0x00},
    {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00},
    {0x0c,0x18,0x30,0x30,0x30,0x18,0x0c,0x00},
    {0x30,0x18,0x0c,0x0c,0x0c,0x18,0x30,0x00},
    {0x00,0x66,0x3c,0xff,0x3c,0x66,0x00,0x00},
    {0x00,0x18,0x18,0x7e,0x18,0x18,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30},
    {0x00,0x00,0x00,0x7e,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00},
    {0x06,0x0c,0x18,0x30,0x60,0xc0,0x80,0x00},
    {0x7c,0xc6,0xce,0xde,0xf6,0xe6,0x7c,0x00},
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7e,0x00},
    {0x7c,0xc6,0x06,0x1c,0x30,0x60,0xfe,0x00},
    {0x7c,0xc6,0x06,0x3c,0x06,0xc6,0x7c,0x00},
    {0x1c,0x3c,0x6c,0xcc,0xfe,0x0c,0x1e,0x00},
    {0xfe,0xc0,0xfc,0x06,0x06,0xc6,0x7c,0x00},
    {0x3c,0x60,0xc0,0xfc,0xc6,0xc6,0x7c,0x00},
    {0xfe,0xc6,0x0c,0x18,0x30,0x30,0x30,0x00},
    {0x7c,0xc6,0xc6,0x7c,0xc6,0xc6,0x7c,0x00},
    {0x7c,0xc6,0xc6,0x7e,0x06,0x0c,0x78,0x00},
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00},
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30},
    {0x0c,0x18,0x30,0x60,0x30,0x18,0x0c,0x00},
    {0x00,0x00,0x7e,0x00,0x7e,0x00,0x00,0x00},
    {0x30,0x18,0x0c,0x06,0x0c,0x18,0x30,0x00},
    {0x7c,0xc6,0x0c,0x18,0x18,0x00,0x18,0x00},
    {0x7c,0xc6,0xde,0xde,0xde,0xc0,0x78,0x00},
    {0x38,0x6c,0xc6,0xc6,0xfe,0xc6,0xc6,0x00},
    {0xfc,0x66,0x66,0x7c,0x66,0x66,0xfc,0x00},
    {0x3c,0x66,0xc0,0xc0,0xc0,0x66,0x3c,0x00},
    {0xf8,0x6c,0x66,0x66,0x66,0x6c,0xf8,0x00},
    {0xfe,0x62,0x68,0x78,0x68,0x62,0xfe,0x00},
    {0xfe,0x62,0x68,0x78,0x68,0x60,0xf0,0x00},
    {0x3c,0x66,0xc0,0xc0,0xce,0x66,0x3e,0x00},
    {0xc6,0xc6,0xc6,0xfe,0xc6,0xc6,0xc6,0x00},
    {0x3c,0x18,0x18,0x18,0x18,0x18,0x3c,0x00},
    {0x1e,0x0c,0x0c,0x0c,0xcc,0xcc,0x78,0x00},
    {0xe6,0x66,0x6c,0x78,0x6c,0x66,0xe6,0x00},
    {0xf0,0x60,0x60,0x60,0x62,0x66,0xfe,0x00},
    {0xc6,0xee,0xfe,0xfe,0xd6,0xc6,0xc6,0x00},
    {0xc6,0xe6,0xf6,0xde,0xce,0xc6,0xc6,0x00},
    {0x7c,0xc6,0xc6,0xc6,0xc6,0xc6,0x7c,0x00},
    {0xfc,0x66,0x66,0x7c,0x60,0x60,0xf0,0x00},
    {0x7c,0xc6,0xc6,0xc6,0xc6,0xd6,0x7c,0x06},
    {0xfc,0x66,0x66,0x7c,0x6c,0x66,0xe6,0x00},
    {0x3c,0x66,0x30,0x18,0x0c,0x66,0x3c,0x00},
    {0x7e,0x5a,0x18,0x18,0x18,0x18,0x3c,0x00},
    {0xc6,0xc6,0xc6,0xc6,0xc6,0xc6,0x7c,0x00},
    {0xc6,0xc6,0xc6,0xc6,0x6c,0x38,0x10,0x00},
    {0xc6,0xc6,0xc6,0xd6,0xfe,0xee,0xc6,0x00},
    {0xc6,0xc6,0x6c,0x38,0x6c,0xc6,0xc6,0x00},
    {0x66,0x66,0x66,0x3c,0x18,0x18,0x3c,0x00},
    {0xfe,0xc6,0x8c,0x18,0x32,0x66,0xfe,0x00},
    {0x3c,0x30,0x30,0x30,0x30,0x30,0x3c,0x00},
    {0xc0,0x60,0x30,0x18,0x0c,0x06,0x02,0x00},
    {0x3c,0x0c,0x0c,0x0c,0x0c,0x0c,0x3c,0x00},
    {0x10,0x38,0x6c,0xc6,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff},
    {0x30,0x18,0x0c,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x78,0x0c,0x7c,0xcc,0x76,0x00},
    {0xe0,0x60,0x7c,0x66,0x66,0x66,0xdc,0x00},
    {0x00,0x00,0x7c,0xc6,0xc0,0xc6,0x7c,0x00},
    {0x1c,0x0c,0x7c,0xcc,0xcc,0xcc,0x76,0x00},
    {0x00,0x00,0x7c,0xc6,0xfe,0xc0,0x7c,0x00},
    {0x3c,0x66,0x60,0xf8,0x60,0x60,0xf0,0x00},
    {0x00,0x00,0x76,0xcc,0xcc,0x7c,0x0c,0x78},
    {0xe0,0x60,0x6c,0x76,0x66,0x66,0xe6,0x00},
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3c,0x00},
    {0x06,0x00,0x0e,0x06,0x06,0x66,0x66,0x3c},
    {0xe0,0x60,0x66,0x6c,0x78,0x6c,0xe6,0x00},
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3c,0x00},
    {0x00,0x00,0xec,0xfe,0xd6,0xd6,0xd6,0x00},
    {0x00,0x00,0xdc,0x66,0x66,0x66,0x66,0x00},
    {0x00,0x00,0x7c,0xc6,0xc6,0xc6,0x7c,0x00},
    {0x00,0x00,0xdc,0x66,0x66,0x7c,0x60,0xf0},
    {0x00,0x00,0x76,0xcc,0xcc,0x7c,0x0c,0x1e},
    {0x00,0x00,0xdc,0x76,0x60,0x60,0xf0,0x00},
    {0x00,0x00,0x7e,0xc0,0x7c,0x06,0xfc,0x00},
    {0x30,0x30,0x7c,0x30,0x30,0x36,0x1c,0x00},
    {0x00,0x00,0xcc,0xcc,0xcc,0xcc,0x76,0x00},
    {0x00,0x00,0xc6,0xc6,0xc6,0x6c,0x38,0x00},
    {0x00,0x00,0xc6,0xd6,0xd6,0xfe,0x6c,0x00},
    {0x00,0x00,0xc6,0x6c,0x38,0x6c,0xc6,0x00},
    {0x00,0x00,0xc6,0xc6,0xce,0x76,0x06,0x7c},
    {0x00,0x00,0xfc,0x98,0x30,0x64,0xfc,0x00},
    {0x1c,0x30,0x30,0x60,0x30,0x30,0x1c,0x00},
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18},
    {0x38,0x0c,0x0c,0x06,0x0c,0x0c,0x38,0x00},
    {0x00,0x00,0x60,0xd2,0x0c,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
};

void leno_gui_platform_draw_text(LenoGUIPlatformRenderer* ren, const char* text, int x, int y, int size) {
    if (!ren || !ren->pixels || !text) return;
    int scale = size > 0 ? size : 1;
    uint32_t color = ((uint32_t)ren->draw_a << 24) | ((uint32_t)ren->draw_r << 16) |
                     ((uint32_t)ren->draw_g << 8) | (uint32_t)ren->draw_b;
    int cx = x + ren->vp_x;
    int cy = y + ren->vp_y;
    const unsigned char* p = (const unsigned char*)text;
    while (*p) {
        int ch = *p++;
        if (ch < 32 || ch > 126) ch = 32;
        const uint8_t* glyph = g_font8x8[ch - 32];
        for (int row = 0; row < 8; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++) {
                if (bits & (0x80 >> col)) {
                    for (int sy = 0; sy < scale; sy++) {
                        for (int sx = 0; sx < scale; sx++) {
                            int px = cx + col * scale + sx;
                            int py = cy + row * scale + sy;
                            if (px >= 0 && px < ren->width && py >= 0 && py < ren->height) {
                                if (ren->clip_enabled) {
                                    if (px < ren->clip_x || px >= ren->clip_x + ren->clip_w ||
                                        py < ren->clip_y || py >= ren->clip_y + ren->clip_h)
                                        continue;
                                }
                                ren->pixels[py * ren->width + px] = color;
                            }
                        }
                    }
                }
            }
        }
        cx += 8 * scale;
    }
}

void leno_gui_platform_text_size(const char* text, int size, int* w, int* h) {
    int scale = size > 0 ? size : 1;
    int len = 0;
    const unsigned char* p = (const unsigned char*)text;
    while (*p) { len++; p++; }
    if (w) *w = len * 8 * scale;
    if (h) *h = 8 * scale;
}

/* ===== 窗口通用操作（操作结构体字段，平台无关） ===== */

int leno_gui_platform_window_should_close(LenoGUIPlatformWindow* win) {
    return win ? win->should_close : 1;
}

void leno_gui_platform_set_window_should_close(LenoGUIPlatformWindow* win, int val) {
    if (win) win->should_close = val;
}

void leno_gui_platform_get_window_size(LenoGUIPlatformWindow* win, int* w, int* h) {
    if (!win) { if (w) *w = 0; if (h) *h = 0; return; }
    if (w) *w = win->width;
    if (h) *h = win->height;
}

/* ===== 主回调状态（参考 SDL3） ===== */

typedef struct {
    LenoGUIPlatformWindow* window;
    LenoGUIPlatformRenderer* renderer;
    LenoGUIRenderCallback render_cb;
    LenoGUIEventCallback event_cb;
    void* user_data;
    int should_quit;
    int needs_redraw;
} LenoGUIMainCallbacks;

static LenoGUIMainCallbacks g_main_callbacks = {0};

static int check_and_resize_renderer(LenoGUIPlatformRenderer* ren);

void leno_gui_platform_set_main_callbacks(LenoGUIPlatformWindow* win,
                                           LenoGUIPlatformRenderer* ren,
                                           LenoGUIRenderCallback render_cb,
                                           LenoGUIEventCallback event_cb,
                                           void* user_data) {
    g_main_callbacks.window = win;
    g_main_callbacks.renderer = ren;
    g_main_callbacks.render_cb = render_cb;
    g_main_callbacks.event_cb = event_cb;
    g_main_callbacks.user_data = user_data;
    g_main_callbacks.should_quit = 0;
    g_main_callbacks.needs_redraw = 1;
}

int leno_gui_platform_iterate_main_callbacks(void) {
    if (g_main_callbacks.should_quit) {
        return 0;
    }

    LenoGUIEvent ev;
    int needs_redraw_from_event = 0;
    while (leno_gui_platform_poll_event(&ev)) {
        if (ev.type == LENO_GUI_EVT_FILEDIALOG_RESULT) {
            /* 文件对话框结果事件：在主线程中处理 */
            leno_gui_platform_process_filedialog_result();
            needs_redraw_from_event = 1;
            continue;
        }

        if (ev.type == LENO_GUI_EVT_WINDOW_RESIZE && g_main_callbacks.renderer) {
            leno_gui_platform_renderer_mark_resize(g_main_callbacks.renderer);
            needs_redraw_from_event = 1;
        }

        if (ev.type == LENO_GUI_EVT_WINDOW_EXPOSED) {
            needs_redraw_from_event = 1;
        }

        if (g_main_callbacks.event_cb) {
            g_main_callbacks.event_cb(g_main_callbacks.user_data, &ev);
        }

        if (ev.type == LENO_GUI_EVT_QUIT || ev.type == LENO_GUI_EVT_WINDOW_CLOSE) {
            g_main_callbacks.should_quit = 1;
        }
    }

    if (g_main_callbacks.window && leno_gui_platform_window_should_close(g_main_callbacks.window)) {
        g_main_callbacks.should_quit = 1;
    }

    if (g_main_callbacks.should_quit) {
        return 0;
    }

    if (needs_redraw_from_event) {
        g_main_callbacks.needs_redraw = 1;
    }

    if (g_main_callbacks.renderer) {
        check_and_resize_renderer(g_main_callbacks.renderer);
    }

    if (g_main_callbacks.needs_redraw && g_main_callbacks.render_cb) {
        g_main_callbacks.render_cb(g_main_callbacks.user_data);
        g_main_callbacks.needs_redraw = 0;
    }

    return 1;
}

void leno_gui_platform_request_redraw(void) {
    g_main_callbacks.needs_redraw = 1;
}

/* ===== 渲染目标（离屏渲染，借鉴 SDL3）===== */

static LenoGUIPlatformTexture* g_current_render_target = NULL;
static LenoGUIPlatformRenderer* g_target_renderer = NULL;
static uint32_t* g_saved_pixels = NULL;
static int g_saved_width = 0;
static int g_saved_height = 0;
static int g_saved_pitch = 0;

int leno_gui_platform_set_render_target(LenoGUIPlatformRenderer* ren, LenoGUIPlatformTexture* tex) {
    if (!ren) return 0;
    
    /* 如果已经有目标，先恢复 */
    if (g_current_render_target) {
        leno_gui_platform_reset_render_target(ren);
    }
    
    if (!tex) {
        /* 设置为空表示渲染到窗口 */
        return 1;
    }
    
    /* 检查纹理是否是渲染目标 */
    if (tex->access != LENO_GUI_TEXTUREACCESS_TARGET) {
        return 0;
    }
    
    /* 保存当前渲染器状态 */
    g_target_renderer = ren;
    g_saved_pixels = ren->pixels;
    g_saved_width = ren->width;
    g_saved_height = ren->height;
    g_saved_pitch = ren->pitch;
    
    /* 切换到纹理 */
    ren->pixels = tex->pixels;
    ren->width = tex->width;
    ren->height = tex->height;
    ren->pitch = tex->pitch;
    
    /* 重置视口和裁剪 */
    ren->vp_x = 0;
    ren->vp_y = 0;
    ren->vp_w = tex->width;
    ren->vp_h = tex->height;
    ren->clip_enabled = 0;
    
    g_current_render_target = tex;
    
    return 1;
}

LenoGUIPlatformTexture* leno_gui_platform_get_render_target(LenoGUIPlatformRenderer* ren) {
    (void)ren;
    return g_current_render_target;
}

void leno_gui_platform_reset_render_target(LenoGUIPlatformRenderer* ren) {
    if (!ren || !g_current_render_target) return;
    
    /* 恢复渲染器状态 */
    ren->pixels = g_saved_pixels;
    ren->width = g_saved_width;
    ren->height = g_saved_height;
    ren->pitch = g_saved_pitch;
    
    /* 重置视口 */
    ren->vp_x = 0;
    ren->vp_y = 0;
    ren->vp_w = g_saved_width;
    ren->vp_h = g_saved_height;
    ren->clip_enabled = 0;
    
    g_current_render_target = NULL;
    g_target_renderer = NULL;
    g_saved_pixels = NULL;
}

void leno_gui_platform_render_target_to_window(LenoGUIPlatformRenderer* ren, LenoGUIPlatformTexture* tex,
                                                int x, int y, int w, int h) {
    if (!ren || !tex || !tex->pixels) return;
    
    /* 保存当前状态 */
    LenoGUIPlatformTexture* old_target = g_current_render_target;
    
    /* 重置到窗口 */
    if (old_target) {
        leno_gui_platform_reset_render_target(ren);
    }
    
    /* 渲染纹理到窗口 */
    if (w <= 0) w = tex->width;
    if (h <= 0) h = tex->height;
    
    leno_gui_platform_render_texture_src(ren, tex, 0, 0, tex->width, tex->height, x, y, w, h);
    
    /* 恢复之前的渲染目标 */
    if (old_target) {
        leno_gui_platform_set_render_target(ren, old_target);
    }
}

void leno_gui_platform_clear_render_target(LenoGUIPlatformTexture* tex, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (!tex || !tex->pixels) return;
    
    uint32_t color = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    int count = tex->width * tex->height;
    for (int i = 0; i < count; i++) {
        tex->pixels[i] = color;
    }
}
