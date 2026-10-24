#!/usr/bin/env python3
"""把 icon/ 下的 .svg / .png 图标批量转换为 LVGL v9 图像资源（C 源文件 + 头文件）。

图标统一按「单色蒙版」处理：
  - .svg 用 resvg 光栅化到 SIZE×SIZE（保持原始比例居中，不拉伸）
  - .png 直接取 Alpha 通道（尺寸必须等于 SIZE）
统一输出 LV_COLOR_FORMAT_A8（1 字节/像素），显示时靠 image_recolor 上色，
零运行时解码、最省 Flash。

同名时优先 .svg（矢量可随时改色/改尺寸重烘），同名 .png 被忽略。

用法：
    python tools/png2lvgl.py

输出：
    main/assets/images/ui_icons.c
    main/assets/images/ui_icons.h
"""

import glob
import io
import os
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DIR = os.path.join(ROOT, "icon")
OUT_DIR = os.path.join(ROOT, "main", "assets", "images")
OUT_C = os.path.join(OUT_DIR, "ui_icons.c")
OUT_H = os.path.join(OUT_DIR, "ui_icons.h")

SIZE = 32           # 图标边长（菜单卡片用）
PER_LINE = 16       # 每行输出的字节数


def c_ident(base):
    """把文件名转成合法的 C 标识符，统一加 ic_ 前缀。"""
    return "ic_" + "".join(ch if ch.isalnum() else "_" for ch in base)


def fit_square(img, size):
    """按比例缩放并居中放进 size×size 透明画布（只缩不放，不拉伸）。"""
    if img.size == (size, size):
        return img

    if img.width > size or img.height > size:
        img = img.copy()
        img.thumbnail((size, size), Image.LANCZOS)

    canvas = Image.new("RGBA", (size, size), (255, 255, 255, 0))
    canvas.alpha_composite(img, ((size - img.width) // 2, (size - img.height) // 2))
    return canvas


def load_alpha(path):
    """返回 (SIZE*SIZE 的 Alpha 蒙版, 素材原始尺寸)。"""
    ext = os.path.splitext(path)[1].lower()

    if ext == ".svg":
        import resvg_py

        with open(path, "r", encoding="utf-8") as f:
            svg_text = f.read()

        # 只指定高度，宽度由 SVG 自身比例推出，避免非正方形图标被拉伸
        png = resvg_py.svg_to_bytes(svg_string=svg_text, height=SIZE)
        img = Image.open(io.BytesIO(png)).convert("RGBA")
    else:
        img = Image.open(path).convert("RGBA")

    src_size = img.size
    alpha = fit_square(img, SIZE).getchannel("A").tobytes()
    return alpha, src_size


def collect_sources():
    """扫描 icon/，按文件名去重，同名优先 .svg。"""
    picked = {}
    for path in sorted(glob.glob(os.path.join(SRC_DIR, "*"))):
        ext = os.path.splitext(path)[1].lower()
        if ext not in (".svg", ".png"):
            continue
        base = os.path.splitext(os.path.basename(path))[0]
        if ext == ".png" and base in picked:
            continue                    # 已有同名 svg
        picked[base] = path
    return picked


def main():
    sources = collect_sources()
    if not sources:
        raise SystemExit("icon/ 下没有 .svg 或 .png 文件")

    masks = {}
    src_sizes = {}
    errors = []
    for base in sorted(sources):
        try:
            masks[base], src_sizes[base] = load_alpha(sources[base])
        except Exception as exc:                      # noqa: BLE001
            errors.append("%s -> %s" % (os.path.basename(sources[base]), exc))

    if errors:
        print("以下图标处理失败，本次不生成任何文件：")
        for e in errors:
            print("  " + e)
        sys.exit(1)

    os.makedirs(OUT_DIR, exist_ok=True)

    c_body = []
    h_body = []
    empty = []

    for base in sorted(masks):
        name = c_ident(base)
        alpha = masks[base]
        if not any(alpha):
            empty.append(base)

        c_body.append("/* %s */" % os.path.basename(sources[base]))
        c_body.append("static const uint8_t %s_map[] = {" % name)
        for i in range(0, len(alpha), PER_LINE):
            row = alpha[i:i + PER_LINE]
            c_body.append("    " + ", ".join("0x%02x" % b for b in row) + ",")
        c_body.append("};")
        c_body.append("")
        c_body.append("const lv_image_dsc_t %s = {" % name)
        c_body.append("    .header.magic = LV_IMAGE_HEADER_MAGIC,")
        c_body.append("    .header.cf = LV_COLOR_FORMAT_A8,")
        c_body.append("    .header.flags = 0,")
        c_body.append("    .header.w = %d," % SIZE)
        c_body.append("    .header.h = %d," % SIZE)
        c_body.append("    .header.stride = %d," % SIZE)
        c_body.append("    .data_size = sizeof(%s_map)," % name)
        c_body.append("    .data = %s_map," % name)
        c_body.append("};")
        c_body.append("")

        h_body.append("LV_IMAGE_DECLARE(%s);" % name)

    with open(OUT_C, "w", encoding="utf-8", newline="\n") as f:
        f.write("/* 由 tools/png2lvgl.py 自动生成，请勿手工修改 */\n\n")
        f.write('#include "lvgl.h"\n')
        f.write('#include "ui_icons.h"\n\n')
        f.write("\n".join(c_body))
        f.write("\n")

    with open(OUT_H, "w", encoding="utf-8", newline="\n") as f:
        f.write("/* 由 tools/png2lvgl.py 自动生成，请勿手工修改 */\n")
        f.write("#pragma once\n\n")
        f.write('#include "lvgl.h"\n\n')
        f.write("#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n")
        f.write("\n".join(h_body))
        f.write("\n\n#ifdef __cplusplus\n}\n#endif\n")

    n_svg = sum(1 for p in sources.values() if p.lower().endswith(".svg"))
    print("已生成 %d 个图标（%d 个来自 SVG，%d 个来自 PNG），输出 %dx%d"
          % (len(masks), n_svg, len(masks) - n_svg, SIZE, SIZE))

    scaled = [b for b in sorted(masks) if src_sizes[b] != (SIZE, SIZE)]
    if scaled:
        print("提示：以下素材不是 %dx%d，已按比例缩放居中（想完全一致可重新导出）：" % (SIZE, SIZE))
        for base in scaled:
            print("  %-26s %dx%d" % (os.path.basename(sources[base]),
                                     src_sizes[base][0], src_sizes[base][1]))

    if empty:
        print("警告：以下图标渲染后全透明，请检查素材（多为 SVG 缺少颜色/尺寸）：")
        for base in empty:
            print("  " + base)

    print("  " + OUT_C)
    print("  " + OUT_H)


if __name__ == "__main__":
    main()
