import pyglet
from pyglet import gl
import os

# ==================== 全局状态 ====================
IMG_W = 0
IMG_H = 0
ZOOM_MIN = 0.02
ZOOM_MAX = 20.0

tex_left = None
tex_right = None

state = {
    "zoom": 1.0,
    "fit_zoom": 1.0,
    "pan_x": 0.0,
    "pan_y": 0.0,
    "split": 0.5,
    "mode": None,
    "hover": "none",
    "shape_mode": "split",   # "split" = 左右分割, "circle" = 圆形对比
    "mouse_x": 0.0,
    "mouse_y": 0.0,
    "circle_scale": 0.25,   # 圆半径占屏比例
}

batch = pyglet.graphics.Batch()

sprite_left = None
sprite_right = None
shape_line = None

window = pyglet.window.Window(
    width=1200, height=800,
    caption="ImageDiff",
    resizable=True,
    file_drops=True
)
window.set_minimum_size(400, 300)
gl.glClearColor(28 / 255, 28 / 255, 28 / 255, 1.0)

# 用窗口当前鼠标位置初始化，避免圆心一开始贴在角落
state["mouse_x"] = getattr(window, "_mouse_x", window.width // 2)
state["mouse_y"] = getattr(window, "_mouse_y", window.height // 2)

# ==================== 自定义 Shader（圆形裁剪） ====================
# vertex 部分照抄 sprite.py 里默认 shader，只增加一个 v_local_pos 传出
CIRCLE_VERTEX_SOURCE = """#version 150 core
    in vec3 translate;
    in vec4 colors;
    in vec3 tex_coords;
    in vec2 scale;
    in vec3 position;
    in float rotation;

    out vec4 vertex_colors;
    out vec3 texture_coords;
    out vec2 v_local_pos;

    uniform WindowBlock
    {
        mat4 projection;
        mat4 view;
    } window;

    mat4 m_scale = mat4(1.0);
    mat4 m_rotation = mat4(1.0);
    mat4 m_translate = mat4(1.0);

    void main()
    {
        m_scale[0][0] = scale.x;
        m_scale[1][1] = scale.y;
        m_translate[3][0] = translate.x;
        m_translate[3][1] = translate.y;
        m_translate[3][2] = translate.z;
        m_rotation[0][0] =  cos(-radians(rotation));
        m_rotation[0][1] =  sin(-radians(rotation));
        m_rotation[1][0] = -sin(-radians(rotation));
        m_rotation[1][1] =  cos(-radians(rotation));

        gl_Position = window.projection * window.view * m_translate * m_rotation * m_scale * vec4(position, 1.0);

        vertex_colors = colors;
        texture_coords = tex_coords;
        v_local_pos = position.xy;
    }
"""

# fragment：use_circle==1 时按圆形 discard，否则原样贴图
CIRCLE_FRAGMENT_SOURCE = """#version 150 core
    in vec4 vertex_colors;
    in vec3 texture_coords;
    in vec2 v_local_pos;
    out vec4 final_colors;

    uniform sampler2D sprite_texture;
    uniform vec2  circle_center;
    uniform float circle_radius;
    uniform float circle_line_width;   // ← 新增：描边宽度（局部像素空间）
    uniform int   use_circle;

    void main()
    {
        if (use_circle == 1) {
            float d = distance(v_local_pos, circle_center);
            // 圆外（含描边外缘）丢弃
            if (d > circle_radius + circle_line_width) {
                discard;
            }
            // 描边环：圆半径外的一圈
            if (d >= circle_radius) {
                final_colors = vec4(1.0, 1.0, 1.0, 1.0);   // 白色实心线
                return;
            }
        }
        final_colors = texture(sprite_texture, texture_coords.xy) * vertex_colors;
    }
"""

circle_program = pyglet.gl.current_context.create_program(
    (CIRCLE_VERTEX_SOURCE, 'vertex'),
    (CIRCLE_FRAGMENT_SOURCE, 'fragment'),
)

# ==================== 核心功能 ====================
def load_texture(image_path):
    if not os.path.exists(image_path):
        return None
    try:
        img = pyglet.image.load(image_path)
        return img.get_texture(), img.width, img.height
    except Exception:
        return None

def redraw():
    global sprite_left, sprite_right, shape_line
    if IMG_W == 0 or IMG_H == 0 or tex_left is None:
        return

    zoom = state["zoom"]
    draw_w = IMG_W * zoom
    draw_h = IMG_H * zoom
    px, py = state["pan_x"], state["pan_y"]
    split_x = px + draw_w * state["split"]
    draw_y = window.height - py - draw_h

    # ---------- 左图 ----------
    if sprite_left is None:
        sprite_left = pyglet.sprite.Sprite(tex_left, x=px, y=draw_y, subpixel=True, batch=batch)
    sprite_left.x = px
    sprite_left.y = draw_y
    sprite_left.scale = zoom

    # ---------- 右图（用自定义 shader） ----------
    if sprite_right is None:
        sprite_right = pyglet.sprite.Sprite(
            tex_right, x=split_x, y=draw_y,
            subpixel=True, batch=batch, program=circle_program,
        )

    if state["shape_mode"] == "circle":
        # 圆形模式：右图完整叠在左图上方，圆外部分由 shader discard
        sprite_right.x = px
        sprite_right.y = draw_y
        sprite_right.scale = zoom
        sprite_right._vertex_list.position[:] = (
            0, 0, 0,
            IMG_W, 0, 0,
            IMG_W, IMG_H, 0,
            0, IMG_H, 0,
        )
        sprite_right._vertex_list.tex_coords[:] = (
            0.0, 0.0, 0,
            1.0, 0.0, 0,
            1.0, 1.0, 0,
            0.0, 1.0, 0,
        )
        if shape_line is not None:
            shape_line.visible = False
    else:
        # 分割模式：右图裁剪成右半边，分割线显示
        sprite_right.x = split_x
        sprite_right.y = draw_y
        sprite_right.scale = zoom
        w_right = IMG_W * (1.0 - state["split"])
        h_right = IMG_H
        sprite_right._vertex_list.position[:] = (
            0, 0, 0,
            w_right, 0, 0,
            w_right, h_right, 0,
            0, h_right, 0,
        )
        u_min = state["split"]
        sprite_right._vertex_list.tex_coords[:] = (
            u_min, 0, 0,
            1.0,   0, 0,
            1.0,   1.0, 0,
            u_min, 1.0, 0,
        )
        if shape_line is None:
            shape_line = pyglet.shapes.Rectangle(split_x, draw_y, 1, draw_h, color=(255, 255, 255), batch=batch)
            shape_line.opacity = 220
        else:
            shape_line.visible = True
            shape_line.x = split_x
            shape_line.y = draw_y
            shape_line.width = 1
            shape_line.height = draw_h

def reset_view():
    if IMG_W == 0: return
    vw, vh = window.width, window.height
    draw_w = IMG_W * state["fit_zoom"]
    draw_h = IMG_H * state["fit_zoom"]
    state["pan_x"] = (vw - draw_w) / 2
    state["pan_y"] = (vh - draw_h) / 2
    state["zoom"] = state["fit_zoom"]
    state["split"] = 0.5
    state["circle_scale"] = 0.25   # Home 复位半径
    state["mode"] = None
    state["hover"] = "none"
    redraw()

def load_pair(path1, path2):
    global IMG_W, IMG_H, tex_left, tex_right, batch
    global sprite_left, sprite_right, shape_line

    res1 = load_texture(path1)
    res2 = load_texture(path2)
    if not res1 or not res2:
        return

    batch = pyglet.graphics.Batch()
    sprite_left = sprite_right = shape_line = None
    state["shape_mode"] = "split"   # 换新图默认回到分割模式

    tex_left,  w1, h1 = res1
    tex_right, w2, h2 = res2
    IMG_W, IMG_H = w1, h1

    vw, vh = window.width, window.height
    state["fit_zoom"] = min(vw / IMG_W, vh / IMG_H, 1.0)
    reset_view()

def toggle_circle_mode():
    if IMG_W == 0:
        return
    state["shape_mode"] = "circle" if state["shape_mode"] == "split" else "split"
    redraw()

# ==================== 事件处理 ====================
@window.event
def on_resize(width, height):
    if IMG_W > 0:
        state["fit_zoom"] = min(width / IMG_W, height / IMG_H, 1.0)
        reset_view()

@window.event
def on_deactivate():
    if state["mode"] is not None:
        state["mode"] = None
        state["hover"] = "none"
        window.set_mouse_cursor(window.get_system_mouse_cursor(window.CURSOR_DEFAULT))

@window.event
def on_mouse_scroll(x, y, scroll_x, scroll_y):
    if scroll_y == 0 or IMG_W == 0: return
    mouse_y = window.height - y
    old_zoom = state["zoom"]
    factor = 1.15 if scroll_y > 0 else 1 / 1.15
    new_zoom = max(ZOOM_MIN, min(ZOOM_MAX, old_zoom * factor))
    if abs(new_zoom - old_zoom) < 1e-9: return

    img_x = (x - state["pan_x"]) / old_zoom
    img_y = (mouse_y - state["pan_y"]) / old_zoom
    state["pan_x"] = x - img_x * new_zoom
    state["pan_y"] = mouse_y - img_y * new_zoom
    state["zoom"] = new_zoom
    redraw()

@window.event
def on_mouse_press(x, y, button, modifiers):
    if button != pyglet.window.mouse.LEFT or IMG_W == 0: return
    mouse_y = window.height - y
    zoom = state["zoom"]
    draw_w = IMG_W * zoom
    draw_h = IMG_H * zoom
    px, py = state["pan_x"], state["pan_y"]
    split_x = px + draw_w * state["split"]

    in_img = (px <= x <= px + draw_w) and (py <= mouse_y <= py + draw_h)
    if not in_img: return

    # 只在分割模式下允许拖分割线
    if state["shape_mode"] == "split" and abs(x - split_x) < 6:
        state["mode"] = "drag_split"
        window.set_mouse_cursor(window.get_system_mouse_cursor(window.CURSOR_SIZE_LEFT_RIGHT))
    else:
        state["mode"] = "drag_view"
        window.set_mouse_cursor(window.get_system_mouse_cursor(window.CURSOR_HAND))

@window.event
def on_mouse_drag(x, y, dx, dy, buttons, modifiers):
    state["mouse_x"] = x
    state["mouse_y"] = y
    if state["mode"] is None or IMG_W == 0: return
    if not (buttons & pyglet.window.mouse.LEFT):
        state["mode"] = None
        return

    if state["mode"] == "drag_view":
        state["pan_x"] += dx
        state["pan_y"] -= dy
        redraw()
    elif state["mode"] == "drag_split":
        zoom = state["zoom"]
        draw_w = IMG_W * zoom
        px = state["pan_x"]
        if draw_w <= 0: return
        split = max(0.0, min(1.0, (x - px) / draw_w))
        if abs(split - state["split"]) > 1e-9:
            state["split"] = split
            redraw()

@window.event
def on_mouse_release(x, y, button, modifiers):
    if button != pyglet.window.mouse.LEFT: return
    if state["mode"] is not None:
        state["mode"] = None
        state["hover"] = "none"
        window.set_mouse_cursor(window.get_system_mouse_cursor(window.CURSOR_DEFAULT))

@window.event
def on_mouse_motion(x, y, dx, dy):
    state["mouse_x"] = x
    state["mouse_y"] = y
    if IMG_W == 0 or state["mode"] is not None:
        return
    mouse_y = window.height - y
    zoom = state["zoom"]
    draw_w = IMG_W * zoom
    draw_h = IMG_H * zoom
    px, py = state["pan_x"], state["pan_y"]
    split_x = px + draw_w * state["split"]

    in_img = (px <= x <= px + draw_w) and (py <= mouse_y <= py + draw_h)

    if state["shape_mode"] == "split" and in_img and abs(x - split_x) < 6:
        if state["hover"] != "split":
            state["hover"] = "split"
            window.set_mouse_cursor(window.get_system_mouse_cursor(window.CURSOR_SIZE_LEFT_RIGHT))
    elif in_img:
        if state["hover"] != "hand":
            state["hover"] = "hand"
            window.set_mouse_cursor(window.get_system_mouse_cursor(window.CURSOR_HAND))
    else:
        if state["hover"] != "none":
            state["hover"] = "none"
            window.set_mouse_cursor(window.get_system_mouse_cursor(window.CURSOR_DEFAULT))

@window.event
def on_key_press(symbol, modifiers):
    key = pyglet.window.key
    if symbol == key.HOME:
        reset_view()
    elif symbol == key.SPACE:
        toggle_circle_mode()
    elif symbol in (key.EQUAL, key.PLUS, key.NUM_ADD):
        # + 键主键盘是 shift+= 即 EQUAL，小键盘是 NUM_ADD
        state["circle_scale"] = min(1.5, state["circle_scale"] * 1.1)
        redraw()
    elif symbol in (key.MINUS, key.NUM_SUBTRACT):
        state["circle_scale"] = max(0.05, state["circle_scale"] / 1.1)
        redraw()

@window.event
def on_file_drop(x, y, paths):
    valid = [p for p in paths if os.path.isfile(p)]
    if len(valid) >= 2:
        load_pair(valid[0], valid[1])

@window.event
def on_draw():
    window.clear()

    # 每帧刷新圆形参数（圆心跟随鼠标）
    if state["shape_mode"] == "circle" and IMG_W > 0:
        zoom = state["zoom"]
        px, py = state["pan_x"], state["pan_y"]
        draw_h = IMG_H * zoom
        draw_y = window.height - py - draw_h

        lx = (state["mouse_x"] - px) / zoom
        ly = (state["mouse_y"] - draw_y) / zoom
        lx = max(0.0, min(float(IMG_W), lx))
        ly = max(0.0, min(float(IMG_H), ly))
        r = min(window.width, window.height) * state["circle_scale"] / zoom   # ← 半径系数

        circle_program.use()
        circle_program['circle_center'] = (lx, ly)
        circle_program['circle_radius'] = r
        circle_program['circle_line_width'] = 1.0 / zoom
        circle_program['use_circle'] = 1
    else:
        circle_program.use()
        circle_program['use_circle'] = 0

    batch.draw()

# ==================== 启动 ====================
if __name__ == "__main__":
    import sys
    if len(sys.argv) >= 3:
        paths = sys.argv[1:3]
        pyglet.clock.schedule_once(lambda dt: load_pair(*paths), 0.0)
    pyglet.app.run()
