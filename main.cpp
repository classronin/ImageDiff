#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>

// ================= 数据层：对象自治 =================
struct ViewState {
    float zoom = 1.f, fit_zoom = 1.f, pan_x = 0.f, pan_y = 0.f;
    float split = 0.5f, circle_scale = 0.25f;
    bool circle_mode = false, has_images = false;
    double mx = 0, my = 0, pmx = 0, pmy = 0;
    bool drag_view = false, drag_split = false;
};
struct Image { GLuint tex = 0; int w = 0, h = 0; };

static Image g_left, g_right;
static ViewState g_st;
static double g_wheel = 0;
static std::string g_pending, g_p1, g_p2;
static bool g_drop_pair = false;
static GLFWcursor *c_arrow, *c_hand, *c_hsize;

// ================= 输入中立：只记录，不处理 =================
static void cb_scroll(GLFWwindow*, double, double y) { g_wheel += y; }
static void cb_drop(GLFWwindow*, int n, const char** p) {
    if (n >= 2) { g_p1 = p[0]; g_p2 = p[1]; g_drop_pair = true; }
    else if (n == 1) {
        if (g_pending.empty()) g_pending = p[0];
        else { g_p1 = g_pending; g_p2 = p[0]; g_pending.clear(); g_drop_pair = true; }
    }
}

// ================= WIC 解码：零异常，失败返回 false =================
static bool decode(const char* utf8, std::vector<unsigned char>& px, int& w, int& h) {
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (len <= 0) return false;
    std::wstring wp(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &wp[0], len);

    IWICImagingFactory* fac = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&fac)))) return false;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frm = nullptr;
    IWICFormatConverter* cvt = nullptr;
    bool ok = false;
    do {
        if (FAILED(fac->CreateDecoderFromFilename(wp.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand, &dec))) break;
        if (FAILED(dec->GetFrame(0, &frm))) break;
        if (FAILED(fac->CreateFormatConverter(&cvt))) break;
        if (FAILED(cvt->Initialize(frm, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                   nullptr, 0.0, WICBitmapPaletteTypeCustom))) break;
        UINT uw, uh; cvt->GetSize(&uw, &uh);
        if (!uw || !uh || uw > 16384 || uh > 16384) break;
        px.resize((size_t)uw * uh * 4);
        if (FAILED(cvt->CopyPixels(nullptr, uw * 4, (UINT)px.size(), px.data()))) break;
        w = (int)uw; h = (int)uh; ok = true;
    } while (false);
    if (cvt) cvt->Release();
    if (frm) frm->Release();
    if (dec) dec->Release();
    fac->Release();
    return ok;
}

static bool upload(const char* path, Image& img) {
    std::vector<unsigned char> px; int w, h;
    if (!decode(path, px, w, h)) return false;
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    if (glGetError() != GL_NO_ERROR) { glDeleteTextures(1, &t); return false; }
    if (img.tex) glDeleteTextures(1, &img.tex);
    img.tex = t; img.w = w; img.h = h;
    return true;
}

static void reset_view(int ww, int wh) {
    if (!g_left.w) return;
    g_st.fit_zoom = std::min({ (float)ww / g_left.w, (float)wh / g_left.h, 1.0f });
    g_st.zoom = g_st.fit_zoom;
    g_st.pan_x = (ww - g_left.w * g_st.zoom) / 2;
    g_st.pan_y = (wh - g_left.h * g_st.zoom) / 2;
    g_st.split = 0.5f; g_st.circle_scale = 0.25f;
    g_st.drag_view = g_st.drag_split = false;
}

// ================= 逻辑层：输入 -> 状态 =================
static void update(GLFWwindow* win, int ww, int wh) {
    ViewState& s = g_st;
    s.pmx = s.mx; s.pmy = s.my;
    glfwGetCursorPos(win, &s.mx, &s.my);
    if (!s.has_images) return;

    float dw = g_left.w * s.zoom, dh = g_left.h * s.zoom;
    float sx = s.pan_x + dw * s.split;
    bool in_img = s.mx >= s.pan_x && s.mx <= s.pan_x + dw && s.my >= s.pan_y && s.my <= s.pan_y + dh;

    if (g_wheel != 0) {
        float old = s.zoom;
        float f = g_wheel > 0 ? 1.15f : 1.f / 1.15f;
        s.zoom = std::max(0.02f, std::min(20.f, old * f));
        s.pan_x = (float)(s.mx - (s.mx - s.pan_x) * (s.zoom / old));
        s.pan_y = (float)(s.my - (s.my - s.pan_y) * (s.zoom / old));
        g_wheel = 0;
    }

    if (glfwGetMouseButton(win, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
        if (!s.drag_view && !s.drag_split && in_img) {
            if (!s.circle_mode && fabs(s.mx - sx) < 6.0) s.drag_split = true;
            else s.drag_view = true;
        }
    } else { s.drag_view = s.drag_split = false; }

    if (s.drag_view) { s.pan_x += (float)(s.mx - s.pmx); s.pan_y += (float)(s.my - s.pmy); }
    else if (s.drag_split && dw > 0) s.split = std::max(0.f, std::min(1.f, (float)(s.mx - s.pan_x) / dw));

    static bool sp = false;
    bool now = glfwGetKey(win, GLFW_KEY_SPACE) == GLFW_PRESS;
    if (now && !sp) s.circle_mode = !s.circle_mode;
    sp = now;

    if (glfwGetKey(win, GLFW_KEY_HOME) == GLFW_PRESS) reset_view(ww, wh);
		if (glfwGetKey(win, GLFW_KEY_EQUAL) == GLFW_PRESS || glfwGetKey(win, GLFW_KEY_KP_ADD) == GLFW_PRESS)
				s.circle_scale = std::min(1.5f, s.circle_scale * 1.03f);
		if (glfwGetKey(win, GLFW_KEY_MINUS) == GLFW_PRESS || glfwGetKey(win, GLFW_KEY_KP_SUBTRACT) == GLFW_PRESS)
				s.circle_scale = std::max(0.05f, s.circle_scale / 1.03f);

    if (s.drag_split || (!s.circle_mode && in_img && fabs(s.mx - sx) < 6.0)) glfwSetCursor(win, c_hsize);
    else if (s.drag_view || in_img) glfwSetCursor(win, c_hand);
    else glfwSetCursor(win, c_arrow);
}

// ================= 渲染层：无脑绘制，不改状态 =================
static void quad(float x, float y, float w, float h) {
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(x, y);
    glTexCoord2f(1, 0); glVertex2f(x + w, y);
    glTexCoord2f(1, 1); glVertex2f(x + w, y + h);
    glTexCoord2f(0, 1); glVertex2f(x, y + h);
    glEnd();
}

static void render(int ww, int wh, int fw, int fh) {
    glViewport(0, 0, fw, fh);
    glClearColor(0.11f, 0.11f, 0.11f, 1.f);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(0, ww, wh, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1, 1, 1, 1);

    const ViewState& s = g_st;

    float dw = g_left.w * s.zoom, dh = g_left.h * s.zoom;
    float px = s.pan_x, py = s.pan_y;
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_left.tex);
    quad(px, py, dw, dh);

    if (s.circle_mode) {
        float cx = (float)s.mx, cy = (float)s.my;
        float r = std::min((float)ww, (float)wh) * s.circle_scale;
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(GL_ALWAYS, 1, 0xFF);
        glStencilOp(GL_REPLACE, GL_REPLACE, GL_REPLACE);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glDisable(GL_TEXTURE_2D);
        glBegin(GL_TRIANGLE_FAN);
        glVertex2f(cx, cy);
        for (int i = 0; i <= 96; ++i) {
            float a = 6.2831853f * i / 96;
            glVertex2f(cx + cosf(a) * r, cy + sinf(a) * r);
        }
        glEnd();
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glStencilFunc(GL_EQUAL, 1, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, g_right.tex);
        quad(px, py, dw, dh);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_TEXTURE_2D);
        glColor3f(1, 1, 1); glLineWidth(2.f);
        glBegin(GL_LINE_LOOP);
        for (int i = 0; i < 96; ++i) {
            float a = 6.2831853f * i / 96;
            glVertex2f(cx + cosf(a) * r, cy + sinf(a) * r);
        }
        glEnd();
    } else {
        float sx = px + dw * s.split;
        float kx = (float)fw / ww, ky = (float)fh / wh;
        glEnable(GL_SCISSOR_TEST);
        glScissor((int)(sx * kx), 0, (int)((ww - sx) * kx) + 1, fh);
        glBindTexture(GL_TEXTURE_2D, g_right.tex);
        quad(px, py, dw, dh);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_TEXTURE_2D);
        glColor3f(1, 1, 1); glLineWidth(1.f);
        glBegin(GL_LINES); glVertex2f(sx, py); glVertex2f(sx, py + dh); glEnd();
    }
    glDisable(GL_TEXTURE_2D);
}

int main(int argc, char** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (!glfwInit()) return -1;
    glfwWindowHint(GLFW_STENCIL_BITS, 8);
    GLFWwindow* win = glfwCreateWindow(1200, 800, "ImageDiff - ClassRonin", nullptr, nullptr);

		// 从 exe 自身资源加载图标（不依赖外部 ico 文件）
		HICON hIcon = (HICON)LoadImageA(GetModuleHandleA(NULL), "IDI_ICON1",
																		 IMAGE_ICON, 0, 0, LR_DEFAULTCOLOR | LR_SHARED);
		if (hIcon) {
				HWND hwnd = glfwGetWin32Window(win);
				SendMessageW(hwnd, WM_SETICON, ICON_BIG,   (LPARAM)hIcon);
				SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);

				ICONINFO ii;
				if (GetIconInfo(hIcon, &ii)) {
						if (ii.hbmColor) {
								HDC hdc = GetDC(NULL);
								BITMAP bm;
								GetObject(ii.hbmColor, sizeof(bm), &bm);
								int w = bm.bmWidth, h = bm.bmHeight;
								std::vector<unsigned char> pixels(w * h * 4);
								
								BITMAPINFO bi = {0};
								bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
								bi.bmiHeader.biWidth = w;
								bi.bmiHeader.biHeight = -h;
								bi.bmiHeader.biPlanes = 1;
								bi.bmiHeader.biBitCount = 32;
								bi.bmiHeader.biCompression = BI_RGB;
								
								GetDIBits(hdc, ii.hbmColor, 0, h, pixels.data(), &bi, DIB_RGB_COLORS);
								ReleaseDC(NULL, hdc);
								
								// 关键：BGRA → RGBA 转换
								for (int i = 0; i < w * h * 4; i += 4) {
										std::swap(pixels[i], pixels[i + 2]);  // 交换 B 和 R
								}
								
								GLFWimage img{w, h, pixels.data()};
								glfwSetWindowIcon(win, 1, &img);
								
								DeleteObject(ii.hbmColor);
								if (ii.hbmMask) DeleteObject(ii.hbmMask);
						}
				}
		}

    if (!win) { glfwTerminate(); return -1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);
    glfwSetScrollCallback(win, cb_scroll);
    glfwSetDropCallback(win, cb_drop);
    c_arrow = glfwCreateStandardCursor(GLFW_ARROW_CURSOR);
    c_hand = glfwCreateStandardCursor(GLFW_HAND_CURSOR);
    c_hsize = glfwCreateStandardCursor(GLFW_HRESIZE_CURSOR);

    if (argc >= 3) { g_p1 = argv[1]; g_p2 = argv[2]; g_drop_pair = true; }

    int last_w = 0, last_h = 0;
    while (!glfwWindowShouldClose(win)) {
        glfwWaitEventsTimeout(1.0 / 60.0);
        int ww, wh, fw, fh;
        glfwGetWindowSize(win, &ww, &wh);
        glfwGetFramebufferSize(win, &fw, &fh);
        if (ww < 1) ww = 1; if (wh < 1) wh = 1;

        if (g_drop_pair) {
            g_drop_pair = false;
            Image a = g_left, b = g_right;
            if (upload(g_p1.c_str(), a) && upload(g_p2.c_str(), b)) {
                g_left = a; g_right = b;
                g_st.has_images = true; g_st.circle_mode = false;
                reset_view(ww, wh);
            } else {
                if (a.tex && a.tex != g_left.tex) glDeleteTextures(1, &a.tex);
                if (b.tex && b.tex != g_right.tex) glDeleteTextures(1, &b.tex);
            }
        }
        if (g_st.has_images && (ww != last_w || wh != last_h)) reset_view(ww, wh);
        last_w = ww; last_h = wh;

        update(win, ww, wh);
        render(ww, wh, fw, fh);
        glfwSwapBuffers(win);
    }
    if (g_left.tex) glDeleteTextures(1, &g_left.tex);
    if (g_right.tex) glDeleteTextures(1, &g_right.tex);
    glfwDestroyCursor(c_arrow); glfwDestroyCursor(c_hand); glfwDestroyCursor(c_hsize);
    glfwDestroyWindow(win);
    glfwTerminate();
    CoUninitialize();
    return 0;
}