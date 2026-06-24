/* Leno GUI - 软件渲染共享代码
 * 三个平台（Win32/Linux/macOS）共用的像素缓冲区渲染逻辑
 * 通过 #include 方式引入，可直接访问平台文件中定义的结构体
 *
 * 包含: 软件渲染辅助函数、绘图 API、视口裁剪、图像操作、文字渲染
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
    if (px < 0 || px >= ren->width || py < 0 || py >= ren->height) return;

    uint8_t a = (color >> 24) & 0xFF;
    if (a == 255) {
        ren->pixels[py * ren->width + px] = color;
    } else if (a > 0) {
        uint8_t r = (color >> 16) & 0xFF;
        uint8_t g = (color >> 8) & 0xFF;
        uint8_t b = color & 0xFF;
        uint32_t dst = ren->pixels[py * ren->width + px];
        uint8_t dr = (dst >> 16) & 0xFF;
        uint8_t dg = (dst >> 8) & 0xFF;
        uint8_t db = dst & 0xFF;
        uint8_t inv = 255 - a;
        dr = (uint8_t)((r * a + dr * inv) / 255);
        dg = (uint8_t)((g * a + dg * inv) / 255);
        db = (uint8_t)((b * a + db * inv) / 255);
        ren->pixels[py * ren->width + px] = LENO_GUI_PIXEL(dr, dg, db, 255);
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

/* ===== 抗锯齿辅助函数 ===== */

/* 抗锯齿绘制单个像素，alpha 范围 [0, 255] */
static void sw_draw_point_aa(LenoGUIPlatformRenderer* ren, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t alpha) {
    int px = x + ren->vp_x;
    int py = y + ren->vp_y;
    if (px < ren->vp_x || px >= ren->vp_x + ren->vp_w) return;
    if (py < ren->vp_y || py >= ren->vp_y + ren->vp_h) return;
    if (ren->clip_enabled) {
        if (px < ren->clip_x || px >= ren->clip_x + ren->clip_w) return;
        if (py < ren->clip_y || py >= ren->clip_y + ren->clip_h) return;
    }
    if (px < 0 || px >= ren->width || py < 0 || py >= ren->height) return;
    if (alpha == 0) return;
    if (alpha == 255) {
        ren->pixels[py * ren->width + px] = LENO_GUI_PIXEL(r, g, b, 255);
        return;
    }
    /* Alpha 混合 */
    uint32_t dst = ren->pixels[py * ren->width + px];
    uint8_t dr = (dst >> 16) & 0xFF;
    uint8_t dg = (dst >> 8) & 0xFF;
    uint8_t db = dst & 0xFF;
    uint8_t inv = 255 - alpha;
    dr = (uint8_t)((r * alpha + dr * inv) / 255);
    dg = (uint8_t)((g * alpha + dg * inv) / 255);
    db = (uint8_t)((b * alpha + db * inv) / 255);
    ren->pixels[py * ren->width + px] = LENO_GUI_PIXEL(dr, dg, db, 255);
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

    uint8_t a = (color >> 24) & 0xFF;
    if (a == 255) {
        for (int row = y1; row < y2; row++) {
            uint32_t* row_ptr = ren->pixels + row * ren->width;
            for (int col = x1; col < x2; col++) {
                row_ptr[col] = color;
            }
        }
    } else if (a > 0) {
        uint8_t r = (color >> 16) & 0xFF;
        uint8_t g = (color >> 8) & 0xFF;
        uint8_t b = color & 0xFF;
        uint8_t inv = 255 - a;
        for (int row = y1; row < y2; row++) {
            uint32_t* row_ptr = ren->pixels + row * ren->width;
            for (int col = x1; col < x2; col++) {
                uint32_t dst = row_ptr[col];
                uint8_t dr = (dst >> 16) & 0xFF;
                uint8_t dg = (dst >> 8) & 0xFF;
                uint8_t db = dst & 0xFF;
                dr = (uint8_t)((r * a + dr * inv) / 255);
                dg = (uint8_t)((g * a + dg * inv) / 255);
                db = (uint8_t)((b * a + db * inv) / 255);
                row_ptr[col] = LENO_GUI_PIXEL(dr, dg, db, 255);
            }
        }
    }
}

static void sw_blit_image(LenoGUIPlatformRenderer* ren,
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

    uint8_t cr = ren->draw_r, cg = ren->draw_g, cb = ren->draw_b, ca = ren->draw_a;

    /* 中间矩形区域（不含圆角部分）直接填充 */
    leno_gui_platform_render_fill_rect(ren, x, y + radius, w, h - 2 * radius);
    leno_gui_platform_render_fill_rect(ren, x + radius, y, w - 2 * radius, radius);
    leno_gui_platform_render_fill_rect(ren, x + radius, y + h - radius, w - 2 * radius, radius);

    /* 抗锯齿圆角：使用浮点距离计算边缘 alpha */
    /* 对每个圆角区域，逐像素计算到圆心的距离，据此决定 alpha */
    int cx_l = x + radius;          /* 左上/左下圆心 x */
    int cx_r = x + w - radius;      /* 右上/右下圆心 x */
    int cy_t = y + radius;          /* 左上/右上圆心 y */
    int cy_b = y + h - radius;      /* 左下/右下圆心 y */

    /* 遍历四个圆角区域的包围盒 */
    for (int iy = 0; iy < radius; iy++) {
        for (int ix = 0; ix < radius; ix++) {
            /* 计算到圆弧的理想距离 */
            float dist = sqrtf((float)(ix * ix + iy * iy)) - (float)(radius - 1);
            if (dist > 1.0f) continue;  /* 完全在圆外 */

            int alpha;
            if (dist <= 0.0f) {
                alpha = 255;  /* 完全在圆内 */
            } else {
                alpha = (int)(255.0f * (1.0f - dist));  /* 抗锯齿过渡 */
            }

            /* 左上角 */
            int px = cx_l - 1 - ix;
            int py = cy_t - 1 - iy;
            sw_draw_point_aa(ren, px, py, cr, cg, cb, (uint8_t)((alpha * ca) / 255));

            /* 右上角 */
            px = cx_r + ix;
            py = cy_t - 1 - iy;
            sw_draw_point_aa(ren, px, py, cr, cg, cb, (uint8_t)((alpha * ca) / 255));

            /* 左下角 */
            px = cx_l - 1 - ix;
            py = cy_b + iy;
            sw_draw_point_aa(ren, px, py, cr, cg, cb, (uint8_t)((alpha * ca) / 255));

            /* 右下角 */
            px = cx_r + ix;
            py = cy_b + iy;
            sw_draw_point_aa(ren, px, py, cr, cg, cb, (uint8_t)((alpha * ca) / 255));
        }
    }
}

/* ===== 高斯模糊阴影（参考 SDL3 两遍可分离模糊） ===== */

/* 在临时缓冲区中绘制圆角矩形形状（仅 alpha 通道） */
static void shadow_draw_rounded_rect(uint8_t* buf, int bw, int bh,
                                      int rx, int ry, int rw, int rh, int radius) {
    if (radius > rw / 2) radius = rw / 2;
    if (radius > rh / 2) radius = rh / 2;
    if (radius <= 0) {
        for (int y = ry; y < ry + rh && y < bh; y++)
            for (int x = rx; x < rx + rw && x < bw; x++)
                buf[y * bw + x] = 255;
        return;
    }
    /* 中间矩形 */
    for (int y = ry + radius; y < ry + rh - radius && y < bh; y++)
        for (int x = rx; x < rx + rw && x < bw; x++)
            if (x >= 0 && y >= 0) buf[y * bw + x] = 255;
    /* 上下矩形 */
    for (int y = ry; y < ry + radius && y < bh; y++)
        for (int x = rx + radius; x < rx + rw - radius && x < bw; x++)
            if (x >= 0 && y >= 0) buf[y * bw + x] = 255;
    for (int y = ry + rh - radius; y < ry + rh && y < bh; y++)
        for (int x = rx + radius; x < rx + rw - radius && x < bw; x++)
            if (x >= 0 && y >= 0) buf[y * bw + x] = 255;
    /* 四个圆角 - 用距离场 */
    int corners[4][2] = {
        {rx + radius, ry + radius},
        {rx + rw - radius, ry + radius},
        {rx + radius, ry + rh - radius},
        {rx + rw - radius, ry + rh - radius}
    };
    for (int c = 0; c < 4; c++) {
        int cx = corners[c][0], cy = corners[c][1];
        for (int dy = -(radius); dy <= radius; dy++) {
            for (int dx = -(radius); dx <= radius; dx++) {
                int px = cx + dx, py = cy + dy;
                if (px < 0 || py < 0 || px >= bw || py >= bh) continue;
                float dist = sqrtf((float)(dx * dx + dy * dy)) - (float)(radius - 1);
                if (dist <= 0.0f) {
                    buf[py * bw + px] = 255;
                } else if (dist < 1.0f) {
                    uint8_t a = buf[py * bw + px];
                    uint8_t na = (uint8_t)(255.0f * (1.0f - dist));
                    if (na > a) buf[py * bw + px] = na;
                }
            }
        }
    }
}

/* 水平方向 box blur（参考 SDL3 的可分离模糊） */
static void blur_h(uint8_t* dst, const uint8_t* src, int w, int h, int radius) {
    if (radius <= 0) { memcpy(dst, src, w * h); return; }
    int diam = radius * 2 + 1;
    for (int y = 0; y < h; y++) {
        int sum = 0;
        /* 初始化窗口 */
        for (int x = -radius; x <= radius; x++) {
            int sx = x < 0 ? 0 : (x >= w ? w - 1 : x);
            sum += src[y * w + sx];
        }
        dst[y * w + 0] = (uint8_t)(sum / diam);
        for (int x = 1; x < w; x++) {
            int add_x = x + radius; if (add_x >= w) add_x = w - 1;
            int rem_x = x - radius - 1; if (rem_x < 0) rem_x = 0;
            sum += src[y * w + add_x] - src[y * w + rem_x];
            dst[y * w + x] = (uint8_t)(sum / diam);
        }
    }
}

/* 垂直方向 box blur */
static void blur_v(uint8_t* dst, const uint8_t* src, int w, int h, int radius) {
    if (radius <= 0) { memcpy(dst, src, w * h); return; }
    int diam = radius * 2 + 1;
    for (int x = 0; x < w; x++) {
        int sum = 0;
        for (int y = -radius; y <= radius; y++) {
            int sy = y < 0 ? 0 : (y >= h ? h - 1 : y);
            sum += src[sy * w + x];
        }
        dst[0 * w + x] = (uint8_t)(sum / diam);
        for (int y = 1; y < h; y++) {
            int add_y = y + radius; if (add_y >= h) add_y = h - 1;
            int rem_y = y - radius - 1; if (rem_y < 0) rem_y = 0;
            sum += src[add_y * w + x] - src[rem_y * w + x];
            dst[y * w + x] = (uint8_t)(sum / diam);
        }
    }
}

/* 绘制圆角矩形阴影（box-shadow 风格，参考 SDL3）
 * 流程：1. 在临时缓冲区绘制圆角矩形形状
 *       2. 三遍 box blur 近似高斯模糊
 *       3. 将模糊结果 alpha 混合到主缓冲区 */
void leno_gui_platform_render_draw_shadow(LenoGUIPlatformRenderer* ren,
    int x, int y, int w, int h, int radius,
    int offset_x, int offset_y, int blur_radius,
    uint8_t sr, uint8_t sg, uint8_t sb, uint8_t sa) {
    if (!ren || !ren->pixels || blur_radius <= 0 || sa <= 0) return;

    /* 阴影矩形区域（含模糊扩展） */
    int sx = x + offset_x - blur_radius;
    int sy = y + offset_y - blur_radius;
    int sw = w + 2 * blur_radius;
    int sh = h + 2 * blur_radius;

    /* 限制缓冲区大小，防止内存爆炸 */
    if (sw > 2000) sw = 2000;
    if (sh > 2000) sh = 2000;
    if (sw <= 0 || sh <= 0) return;

    int buf_size = sw * sh;
    uint8_t* buf_a = (uint8_t*)malloc(buf_size);   /* alpha 缓冲区 */
    uint8_t* buf_b = (uint8_t*)malloc(buf_size);   /* 临时缓冲区 */
    if (!buf_a || !buf_b) { free(buf_a); free(buf_b); return; }
    memset(buf_a, 0, buf_size);

    /* 1. 在临时缓冲区中绘制圆角矩形形状 */
    int shape_x = blur_radius;   /* 形状在缓冲区中的位置 */
    int shape_y = blur_radius;
    shadow_draw_rounded_rect(buf_a, sw, sh, shape_x, shape_y, w, h, radius);

    /* 2. 三遍 box blur 近似高斯模糊（参考 SDL3） */
    int blur_r = blur_radius / 3;
    if (blur_r < 1) blur_r = 1;
    /* Pass 1: H */
    blur_h(buf_b, buf_a, sw, sh, blur_r);
    /* Pass 2: V */
    blur_v(buf_a, buf_b, sw, sh, blur_r);
    /* Pass 3: H */
    blur_h(buf_b, buf_a, sw, sh, blur_r);
    /* Pass 4: V (最终结果在 buf_b) */
    blur_v(buf_a, buf_b, sw, sh, blur_r);

    /* 3. 将模糊结果 alpha 混合到主帧缓冲区 */
    for (int by = 0; by < sh; by++) {
        int py = sy + by + ren->vp_y;
        if (py < 0 || py >= ren->height) continue;
        if (ren->clip_enabled && (py < ren->clip_y || py >= ren->clip_y + ren->clip_h)) continue;
        for (int bx = 0; bx < sw; bx++) {
            int px = sx + bx + ren->vp_x;
            if (px < 0 || px >= ren->width) continue;
            if (ren->clip_enabled && (px < ren->clip_x || px >= ren->clip_x + ren->clip_w)) continue;

            uint8_t alpha = buf_a[by * sw + bx];
            if (alpha == 0) continue;
            /* 应用阴影颜色的 alpha */
            uint8_t final_a = (uint8_t)((alpha * sa) / 255);
            if (final_a == 0) continue;

            /* Alpha 混合到主缓冲区 */
            uint32_t dst = ren->pixels[py * ren->width + px];
            uint8_t dr = (dst >> 16) & 0xFF;
            uint8_t dg = (dst >> 8) & 0xFF;
            uint8_t db = dst & 0xFF;
            uint8_t inv = 255 - final_a;
            dr = (uint8_t)((sr * final_a + dr * inv) / 255);
            dg = (uint8_t)((sg * final_a + dg * inv) / 255);
            db = (uint8_t)((sb * final_a + db * inv) / 255);
            ren->pixels[py * ren->width + px] = LENO_GUI_PIXEL(dr, dg, db, 255);
        }
    }

    free(buf_a);
    free(buf_b);
}

/* ===== 渐变填充 ===== */

/* 水平线性渐变矩形填充 */
void leno_gui_platform_render_fill_gradient_rect(LenoGUIPlatformRenderer* ren,
    int x, int y, int w, int h,
    int count, const uint8_t* r, const uint8_t* g, const uint8_t* b, const uint8_t* a,
    int opacity) {
    if (!ren || !ren->pixels || w <= 0 || h <= 0 || count < 2) return;

    /* 计算有效区域 */
    int x1 = x + ren->vp_x;
    int y1 = y + ren->vp_y;
    int x2 = x1 + w;
    int y2 = y1 + h;
    if (x1 < ren->vp_x) x1 = ren->vp_x;
    if (y1 < ren->vp_y) y1 = ren->vp_y;
    if (x2 > ren->vp_x + ren->vp_w) x2 = ren->vp_x + ren->vp_w;
    if (y2 > ren->vp_y + ren->vp_h) y2 = ren->vp_y + ren->vp_h;
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

    int segments = count - 1;
    float seg_w = (float)w / segments;

    for (int row = y1; row < y2; row++) {
        uint32_t* row_ptr = ren->pixels + row * ren->width;
        for (int col = x1; col < x2; col++) {
            /* 计算当前列在渐变中的位置 [0, segments] */
            float pos = (float)(col - x - ren->vp_x) / seg_w;
            if (pos < 0) pos = 0;
            if (pos > segments) pos = segments;

            int idx = (int)pos;
            if (idx >= segments) idx = segments - 1;
            float t = pos - idx;

            uint8_t rr = (uint8_t)(r[idx] * (1 - t) + r[idx + 1] * t);
            uint8_t gg = (uint8_t)(g[idx] * (1 - t) + g[idx + 1] * t);
            uint8_t bb = (uint8_t)(b[idx] * (1 - t) + b[idx + 1] * t);
            uint8_t aa = (uint8_t)(a[idx] * (1 - t) + a[idx + 1] * t);

            if (opacity < 255) aa = (aa * opacity) / 255;

            if (aa == 255) {
                row_ptr[col] = LENO_GUI_PIXEL(rr, gg, bb, 255);
            } else if (aa > 0) {
                uint32_t dst = row_ptr[col];
                uint8_t dr = (dst >> 16) & 0xFF;
                uint8_t dg = (dst >> 8) & 0xFF;
                uint8_t db = dst & 0xFF;
                uint8_t inv = 255 - aa;
                dr = (uint8_t)((rr * aa + dr * inv) / 255);
                dg = (uint8_t)((gg * aa + dg * inv) / 255);
                db = (uint8_t)((bb * aa + db * inv) / 255);
                row_ptr[col] = LENO_GUI_PIXEL(dr, dg, db, 255);
            }
        }
    }
}

/* ===== 圆角渐变填充 ===== */

/* 辅助：计算渐变颜色并 alpha 混合到单个像素 */
static void sw_gradient_pixel(LenoGUIPlatformRenderer* ren, int px, int py,
    int count, const uint8_t* r, const uint8_t* g, const uint8_t* b, const uint8_t* a,
    float pos, int opacity) {
    if (pos < 0) pos = 0;
    int segments = count - 1;
    if (pos > segments) pos = segments;
    int idx = (int)pos;
    if (idx >= segments) idx = segments - 1;
    float t = pos - idx;
    uint8_t rr = (uint8_t)(r[idx] * (1.0f - t) + r[idx + 1] * t);
    uint8_t gg = (uint8_t)(g[idx] * (1.0f - t) + g[idx + 1] * t);
    uint8_t bb = (uint8_t)(b[idx] * (1.0f - t) + b[idx + 1] * t);
    uint8_t aa = (uint8_t)(a[idx] * (1.0f - t) + a[idx + 1] * t);
    if (opacity < 255) aa = (aa * opacity) / 255;
    if (aa == 0) return;
    if (aa == 255) {
        ren->pixels[py * ren->width + px] = LENO_GUI_PIXEL(rr, gg, bb, 255);
    } else {
        uint32_t dst = ren->pixels[py * ren->width + px];
        uint8_t dr = (dst >> 16) & 0xFF;
        uint8_t dg = (dst >> 8) & 0xFF;
        uint8_t db = dst & 0xFF;
        uint8_t inv = 255 - aa;
        dr = (uint8_t)((rr * aa + dr * inv) / 255);
        dg = (uint8_t)((gg * aa + dg * inv) / 255);
        db = (uint8_t)((bb * aa + db * inv) / 255);
        ren->pixels[py * ren->width + px] = LENO_GUI_PIXEL(dr, dg, db, 255);
    }
}

/* 圆角线性渐变矩形（vertical=0 水平，vertical=1 垂直） */
void leno_gui_platform_render_fill_gradient_rounded_rect(LenoGUIPlatformRenderer* ren,
    int x, int y, int w, int h, int radius,
    int count, const uint8_t* r, const uint8_t* g, const uint8_t* b, const uint8_t* a,
    int opacity, int vertical) {
    if (!ren || !ren->pixels || w <= 0 || h <= 0 || count < 2) return;
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;

    int x1 = x + ren->vp_x, y1 = y + ren->vp_y;
    int x2 = x1 + w, y2 = y1 + h;
    if (x1 < ren->vp_x) x1 = ren->vp_x;
    if (y1 < ren->vp_y) y1 = ren->vp_y;
    if (x2 > ren->vp_x + ren->vp_w) x2 = ren->vp_x + ren->vp_w;
    if (y2 > ren->vp_y + ren->vp_h) y2 = ren->vp_y + ren->vp_h;
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

    int segments = count - 1;
    float seg_len = vertical ? (float)h / segments : (float)w / segments;
    int cx_l = x + ren->vp_x + radius, cx_r = x + ren->vp_x + w - radius;
    int cy_t = y + ren->vp_y + radius, cy_b = y + ren->vp_y + h - radius;

    for (int row = y1; row < y2; row++) {
        for (int col = x1; col < x2; col++) {
            if (radius > 0) {
                int in_corner = 0, ix = 0, iy = 0;
                if (col < cx_l && row < cy_t) { ix = cx_l - 1 - col; iy = cy_t - 1 - row; in_corner = 1; }
                else if (col >= cx_r && row < cy_t) { ix = col - cx_r; iy = cy_t - 1 - row; in_corner = 1; }
                else if (col < cx_l && row >= cy_b) { ix = cx_l - 1 - col; iy = row - cy_b; in_corner = 1; }
                else if (col >= cx_r && row >= cy_b) { ix = col - cx_r; iy = row - cy_b; in_corner = 1; }
                if (in_corner) {
                    float dist = sqrtf((float)(ix * ix + iy * iy)) - (float)(radius - 1);
                    if (dist > 1.0f) continue;
                }
            }
            float pos = vertical ? (float)(row - y - ren->vp_y) / seg_len : (float)(col - x - ren->vp_x) / seg_len;
            sw_gradient_pixel(ren, col, row, count, r, g, b, a, pos, opacity);
        }
    }
}

/* 径向渐变矩形 */
void leno_gui_platform_render_fill_gradient_radial_rect(LenoGUIPlatformRenderer* ren,
    int x, int y, int w, int h, int radius,
    int count, const uint8_t* r, const uint8_t* g, const uint8_t* b, const uint8_t* a,
    int opacity) {
    if (!ren || !ren->pixels || w <= 0 || h <= 0 || count < 2) return;
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;

    int x1 = x + ren->vp_x, y1 = y + ren->vp_y;
    int x2 = x1 + w, y2 = y1 + h;
    if (x1 < ren->vp_x) x1 = ren->vp_x;
    if (y1 < ren->vp_y) y1 = ren->vp_y;
    if (x2 > ren->vp_x + ren->vp_w) x2 = ren->vp_x + ren->vp_w;
    if (y2 > ren->vp_y + ren->vp_h) y2 = ren->vp_y + ren->vp_h;
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

    int segments = count - 1;
    int cx = x + ren->vp_x + w / 2, cy = y + ren->vp_y + h / 2;
    float max_dist = sqrtf((float)(w * w + h * h)) / 2.0f;
    int cx_l = x + ren->vp_x + radius, cx_r = x + ren->vp_x + w - radius;
    int cy_t = y + ren->vp_y + radius, cy_b = y + ren->vp_y + h - radius;

    for (int row = y1; row < y2; row++) {
        for (int col = x1; col < x2; col++) {
            if (radius > 0) {
                int in_corner = 0, ix = 0, iy = 0;
                if (col < cx_l && row < cy_t) { ix = cx_l - 1 - col; iy = cy_t - 1 - row; in_corner = 1; }
                else if (col >= cx_r && row < cy_t) { ix = col - cx_r; iy = cy_t - 1 - row; in_corner = 1; }
                else if (col < cx_l && row >= cy_b) { ix = cx_l - 1 - col; iy = row - cy_b; in_corner = 1; }
                else if (col >= cx_r && row >= cy_b) { ix = col - cx_r; iy = row - cy_b; in_corner = 1; }
                if (in_corner) {
                    float dist = sqrtf((float)(ix * ix + iy * iy)) - (float)(radius - 1);
                    if (dist > 1.0f) continue;
                }
            }
            float dxx = (float)(col - cx), dyy = (float)(row - cy);
            float dist = sqrtf(dxx * dxx + dyy * dyy);
            float pos = (dist / max_dist) * segments;
            sw_gradient_pixel(ren, col, row, count, r, g, b, a, pos, opacity);
        }
    }
}

/* ===== 几何图形扩展（椭圆、三角形、多边形、圆弧） ===== */

void leno_gui_platform_render_draw_ellipse(LenoGUIPlatformRenderer* ren, int cx, int cy, int rx, int ry) {
    if (!ren || !ren->pixels || rx <= 0 || ry <= 0) return;
    uint32_t color = LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a);
    int rx2 = rx * rx;
    int ry2 = ry * ry;
    int x = 0, y = ry;
    int d = ry2 - rx2 * ry + rx2 / 4;
    while (ry2 * x <= rx2 * y) {
        sw_draw_point(ren, cx + x, cy + y, color);
        sw_draw_point(ren, cx - x, cy + y, color);
        sw_draw_point(ren, cx + x, cy - y, color);
        sw_draw_point(ren, cx - x, cy - y, color);
        x++;
        if (d < 0) {
            d += 2 * ry2 * x + ry2;
        } else {
            y--;
            d += 2 * ry2 * x - 2 * rx2 * y + ry2;
        }
    }
    d = ry2 * (x + 0.5) * (x + 0.5) + rx2 * (y - 1) * (y - 1) - rx2 * ry2;
    while (y >= 0) {
        sw_draw_point(ren, cx + x, cy + y, color);
        sw_draw_point(ren, cx - x, cy + y, color);
        sw_draw_point(ren, cx + x, cy - y, color);
        sw_draw_point(ren, cx - x, cy - y, color);
        y--;
        if (d > 0) {
            d -= 2 * rx2 * y + rx2;
        } else {
            x++;
            d += 2 * ry2 * x - 2 * rx2 * y + rx2;
        }
    }
}

void leno_gui_platform_render_fill_ellipse(LenoGUIPlatformRenderer* ren, int cx, int cy, int rx, int ry) {
    if (!ren || !ren->pixels || rx <= 0 || ry <= 0) return;
    uint32_t color = LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a);
    for (int y = -ry; y <= ry; y++) {
        int x = (int)(rx * sqrt(1.0 - (double)(y * y) / (ry * ry)));
        sw_draw_line(ren, cx - x, cy + y, cx + x, cy + y, color);
    }
}

void leno_gui_platform_render_draw_arc(LenoGUIPlatformRenderer* ren, int cx, int cy, int r, double start_angle, double end_angle) {
    if (!ren || !ren->pixels || r <= 0) return;
    uint32_t color = LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a);
    const double PI = 3.14159265358979323846;
    double start = start_angle * PI / 180.0;
    double end = end_angle * PI / 180.0;
    if (end < start) end += 2 * PI;
    int steps = (int)(r * (end - start));
    if (steps < 10) steps = 10;
    double prev_x = cx + r * cos(start);
    double prev_y = cy + r * sin(start);
    for (int i = 1; i <= steps; i++) {
        double t = start + (end - start) * i / steps;
        double x = cx + r * cos(t);
        double y = cy + r * sin(t);
        sw_draw_line(ren, (int)prev_x, (int)prev_y, (int)x, (int)y, color);
        prev_x = x;
        prev_y = y;
    }
}

void leno_gui_platform_render_draw_triangle(LenoGUIPlatformRenderer* ren, int x1, int y1, int x2, int y2, int x3, int y3) {
    if (!ren || !ren->pixels) return;
    uint32_t color = LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a);
    sw_draw_line(ren, x1, y1, x2, y2, color);
    sw_draw_line(ren, x2, y2, x3, y3, color);
    sw_draw_line(ren, x3, y3, x1, y1, color);
}

static void sw_fill_flat_bottom_triangle(LenoGUIPlatformRenderer* ren, int x1, int y1, int x2, int y2, int x3, int y3, uint32_t color) {
    float slope1 = (float)(x2 - x1) / (y2 - y1);
    float slope2 = (float)(x3 - x1) / (y3 - y1);
    float x_start = x1;
    float x_end = x1;
    for (int y = y1; y <= y2; y++) {
        sw_draw_line(ren, (int)x_start, y, (int)x_end, y, color);
        x_start += slope1;
        x_end += slope2;
    }
}

static void sw_fill_flat_top_triangle(LenoGUIPlatformRenderer* ren, int x1, int y1, int x2, int y2, int x3, int y3, uint32_t color) {
    float slope1 = (float)(x1 - x3) / (y1 - y3);
    float slope2 = (float)(x2 - x3) / (y2 - y3);
    float x_start = x3;
    float x_end = x3;
    for (int y = y3; y >= y1; y--) {
        sw_draw_line(ren, (int)x_start, y, (int)x_end, y, color);
        x_start -= slope1;
        x_end -= slope2;
    }
}

void leno_gui_platform_render_fill_triangle(LenoGUIPlatformRenderer* ren, int x1, int y1, int x2, int y2, int x3, int y3) {
    if (!ren || !ren->pixels) return;
    uint32_t color = LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a);
    /* 按 y 坐标排序 */
    if (y1 > y2) { int t = x1; x1 = x2; x2 = t; t = y1; y1 = y2; y2 = t; }
    if (y1 > y3) { int t = x1; x1 = x3; x3 = t; t = y1; y1 = y3; y3 = t; }
    if (y2 > y3) { int t = x2; x2 = x3; x3 = t; t = y2; y2 = y3; y3 = t; }
    if (y2 == y3) {
        sw_fill_flat_bottom_triangle(ren, x1, y1, x2, y2, x3, y3, color);
    } else if (y1 == y2) {
        sw_fill_flat_top_triangle(ren, x1, y1, x2, y2, x3, y3, color);
    } else {
        int x4 = x1 + (int)((float)(y2 - y1) / (y3 - y1) * (x3 - x1));
        sw_fill_flat_bottom_triangle(ren, x1, y1, x2, y2, x4, y2, color);
        sw_fill_flat_top_triangle(ren, x2, y2, x4, y2, x3, y3, color);
    }
}

void leno_gui_platform_render_draw_polygon(LenoGUIPlatformRenderer* ren, const int* points, int num_points) {
    if (!ren || !ren->pixels || num_points < 3) return;
    uint32_t color = LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a);
    for (int i = 0; i < num_points; i++) {
        int x1 = points[i * 2];
        int y1 = points[i * 2 + 1];
        int x2 = points[((i + 1) % num_points) * 2];
        int y2 = points[((i + 1) % num_points) * 2 + 1];
        sw_draw_line(ren, x1, y1, x2, y2, color);
    }
}

void leno_gui_platform_render_fill_polygon(LenoGUIPlatformRenderer* ren, const int* points, int num_points) {
    if (!ren || !ren->pixels || num_points < 3) return;
    uint32_t color = LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a);
    /* 扫描线填充算法 */
    int min_y = points[1], max_y = points[1];
    for (int i = 1; i < num_points; i++) {
        if (points[i * 2 + 1] < min_y) min_y = points[i * 2 + 1];
        if (points[i * 2 + 1] > max_y) max_y = points[i * 2 + 1];
    }
    for (int y = min_y; y <= max_y; y++) {
        int intersections[64];
        int count = 0;
        for (int i = 0; i < num_points && count < 64; i++) {
            int x1 = points[i * 2], y1 = points[i * 2 + 1];
            int x2 = points[((i + 1) % num_points) * 2], y2 = points[((i + 1) % num_points) * 2 + 1];
            if ((y1 <= y && y2 > y) || (y2 <= y && y1 > y)) {
                float t = (float)(y - y1) / (y2 - y1);
                intersections[count++] = (int)(x1 + t * (x2 - x1));
            }
        }
        /* 冒泡排序交点 */
        for (int i = 0; i < count - 1; i++) {
            for (int j = 0; j < count - i - 1; j++) {
                if (intersections[j] > intersections[j + 1]) {
                    int t = intersections[j];
                    intersections[j] = intersections[j + 1];
                    intersections[j + 1] = t;
                }
            }
        }
        for (int i = 0; i < count - 1; i += 2) {
            sw_draw_line(ren, intersections[i], y, intersections[i + 1], y, color);
        }
    }
}

void leno_gui_platform_render_draw_bezier(LenoGUIPlatformRenderer* ren, const int* points, int num_points, int steps) {
    if (!ren || !ren->pixels || num_points < 3 || steps < 2) return;
    uint32_t color = LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a);
    if (steps < 10) steps = 10;
    double prev_x = points[0];
    double prev_y = points[1];
    for (int i = 1; i <= steps; i++) {
        double t = (double)i / steps;
        double x = 0, y = 0;
        /* De Casteljau 算法计算贝塞尔曲线点 */
        double temp[16][2];
        int n = num_points;
        if (n > 16) n = 16;
        for (int j = 0; j < n; j++) {
            temp[j][0] = points[j * 2];
            temp[j][1] = points[j * 2 + 1];
        }
        for (int r = 1; r < n; r++) {
            for (int j = 0; j < n - r; j++) {
                temp[j][0] = (1 - t) * temp[j][0] + t * temp[j + 1][0];
                temp[j][1] = (1 - t) * temp[j][1] + t * temp[j + 1][1];
            }
        }
        x = temp[0][0];
        y = temp[0][1];
        sw_draw_line(ren, (int)prev_x, (int)prev_y, (int)x, (int)y, color);
        prev_x = x;
        prev_y = y;
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
#ifdef _WIN32
    if (ren->back_dc) {
        int cx = x + ren->vp_x, cy = y + ren->vp_y;
        HRGN rgn = CreateRectRgn(cx, cy, cx + w, cy + h);
        if (rgn) {
            SelectClipRgn(ren->back_dc, rgn);
            DeleteObject(rgn);
        }
    }
#endif
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
#ifdef _WIN32
    if (ren->back_dc) {
        SelectClipRgn(ren->back_dc, NULL);
    }
#endif
}

/* ===== 图像操作 ===== */

LenoGUIPlatformImage* leno_gui_platform_create_image(LenoGUIPlatformRenderer* ren, int w, int h) {
    (void)ren;
    LenoGUIPlatformImage* tex = (LenoGUIPlatformImage*)calloc(1, sizeof(LenoGUIPlatformImage));
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

void leno_gui_platform_destroy_image(LenoGUIPlatformImage* tex) {
    if (!tex) return;
    if (tex->pixels) free(tex->pixels);
    free(tex);
}

void leno_gui_platform_render_image(LenoGUIPlatformRenderer* ren, LenoGUIPlatformImage* tex, int x, int y) {
    if (!ren || !ren->pixels || !tex || !tex->pixels) return;
    sw_blit_image(ren, tex->pixels, tex->width, tex->height, tex->pitch, x, y);
}

void leno_gui_platform_update_image(LenoGUIPlatformImage* tex, const void* data, int pitch) {
    if (!tex || !tex->pixels || !data) return;
    const uint32_t* src = (const uint32_t*)data;
    for (int y = 0; y < tex->height; y++) {
        memcpy(tex->pixels + y * tex->width, (const uint8_t*)src + y * pitch, tex->width * 4);
    }
}

int leno_gui_platform_image_width(LenoGUIPlatformImage* tex) {
    return tex ? tex->width : 0;
}

int leno_gui_platform_image_height(LenoGUIPlatformImage* tex) {
    return tex ? tex->height : 0;
}

int leno_gui_platform_image_access(LenoGUIPlatformImage* tex) {
    return tex ? tex->access : 0;
}

void leno_gui_platform_render_image_src(LenoGUIPlatformRenderer* ren, LenoGUIPlatformImage* tex,
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

/* 简单位图源矩形翻转绘制 - 水平翻转 */
void leno_gui_platform_render_image_src_flipped(LenoGUIPlatformRenderer* ren, LenoGUIPlatformImage* tex,
                                                  int sx, int sy, int sw, int sh,
                                                  int dx, int dy, int dw, int dh) {
    if (!ren || !ren->pixels || !tex || !tex->pixels) return;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    float x_scale = (float)sw / (float)dw;
    float y_scale = (float)sh / (float)dh;
    int src_pitch_int = tex->pitch / 4;
    for (int row = 0; row < dh; row++) {
        int src_y = sy + (int)(row * y_scale);
        if (src_y < 0 || src_y >= tex->height) continue;
        for (int col = 0; col < dw; col++) {
            int src_x = sx + (int)((dw - 1 - col) * x_scale);
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

void leno_gui_platform_render_image_rotated(LenoGUIPlatformRenderer* ren, LenoGUIPlatformImage* tex,
                                               int x, int y, double angle, int flip) {
    int w = tex ? tex->width : 0;
    int h = tex ? tex->height : 0;
    leno_gui_platform_render_image_rotated_src(ren, tex, 0, 0, w, h, x, y, w, h, angle, flip);
}

void leno_gui_platform_render_image_rotated_src(LenoGUIPlatformRenderer* ren, LenoGUIPlatformImage* tex,
                                                   int sx, int sy, int sw, int sh,
                                                   int dx, int dy, int dw, int dh,
                                                   double angle, int flip) {
    if (!ren || !ren->pixels || !tex || !tex->pixels) return;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    double rad = angle * 3.14159265358979323846 / 180.0;
    double cos_a = cos(rad);
    double sin_a = sin(rad);
    int tw = tex->width;
    int th = tex->height;
    int src_pitch_int = tex->pitch / 4;
    double scale_x = (double)dw / sw;   /* 目标→源比例，现在用源图尺寸缩放 */
    double scale_y = (double)dh / sh;
    double src_cx = sx + sw / 2.0;
    double src_cy = sy + sh / 2.0;

    /* 计算旋转后的包围盒：源图四个角旋转后的范围 */
    double abs_cos = fabs(cos_a);
    double abs_sin = fabs(sin_a);
    double hsw = sw * 0.5, hsh = sh * 0.5;
    /* 包围盒半宽 = |cos|*hsw + |sin|*hsh，半高 = |sin|*hsw + |cos|*hsh */
    double bhw = (abs_cos * hsw + abs_sin * hsh) * scale_x;
    double bhh = (abs_sin * hsw + abs_cos * hsh) * scale_y;
    int ibhw = (int)(bhw + 1.0);
    int ibhh = (int)(bhh + 1.0);

    /* 中心点 */
    int ctx = dx + dw / 2;
    int cty = dy + dh / 2;

    /* 逆缩放因子（目标→源） */
    double inv_scale_x = (double)sw / dw;
    double inv_scale_y = (double)sh / dh;

    for (int ody = -ibhh; ody <= ibhh; ody++) {
        for (int odx = -ibhw; odx <= ibhw; odx++) {
            double ofx = (double)odx, ofy = (double)ody;
            if (flip & LENO_GUI_FLIP_HORIZONTAL) ofx = -ofx;
            if (flip & LENO_GUI_FLIP_VERTICAL) ofy = -ofy;
            /* 逆旋转变换：从目标坐标映射回源坐标 */
            double src_x = (cos_a * ofx - sin_a * ofy) * inv_scale_x + src_cx;
            double src_y = (sin_a * ofx + cos_a * ofy) * inv_scale_y + src_cy;
            int isx = (int)(src_x + 0.5);
            int isy = (int)(src_y + 0.5);
            if (isx < 0 || isx >= tw || isy < 0 || isy >= th) continue;
            uint32_t src_pixel = tex->pixels[isy * src_pitch_int + isx];
            uint8_t sa = (src_pixel >> 24) & 0xFF;
            if (sa == 0) continue;
            sw_draw_point(ren, ctx + odx, cty + ody, src_pixel);
        }
    }
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

static LenoGUIPlatformImage* g_current_render_target = NULL;
static LenoGUIPlatformRenderer* g_target_renderer = NULL;
static uint32_t* g_saved_pixels = NULL;
static int g_saved_width = 0;
static int g_saved_height = 0;
static int g_saved_pitch = 0;

int leno_gui_platform_set_render_target(LenoGUIPlatformRenderer* ren, LenoGUIPlatformImage* tex) {
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
    if (tex->access != LENO_GUI_IMAGEACCESS_TARGET) {
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

LenoGUIPlatformImage* leno_gui_platform_get_render_target(LenoGUIPlatformRenderer* ren) {
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

void leno_gui_platform_render_target_to_window(LenoGUIPlatformRenderer* ren, LenoGUIPlatformImage* tex,
                                                int x, int y, int w, int h) {
    if (!ren || !tex || !tex->pixels) return;
    
    /* 保存当前状态 */
    LenoGUIPlatformImage* old_target = g_current_render_target;
    
    /* 重置到窗口 */
    if (old_target) {
        leno_gui_platform_reset_render_target(ren);
    }
    
    /* 渲染纹理到窗口 */
    if (w <= 0) w = tex->width;
    if (h <= 0) h = tex->height;
    
    leno_gui_platform_render_image_src(ren, tex, 0, 0, tex->width, tex->height, x, y, w, h);
    
    /* 恢复之前的渲染目标 */
    if (old_target) {
        leno_gui_platform_set_render_target(ren, old_target);
    }
}

void leno_gui_platform_clear_render_target(LenoGUIPlatformImage* tex, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (!tex || !tex->pixels) return;
    
    uint32_t color = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    int count = tex->width * tex->height;
    for (int i = 0; i < count; i++) {
        tex->pixels[i] = color;
    }
}
