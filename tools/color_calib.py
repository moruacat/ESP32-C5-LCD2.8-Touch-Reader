#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""屏幕色彩校准分析

配合固件里的校准页（串口输入 `cal` 进入）使用：
屏幕整屏铺一个纯色，四角各放一个定位二维码。用手机把屏幕拍下来，
本脚本识别四角二维码做透视校正（所以拍照角度、距离、拍歪都无所谓），
再把校准色区域的平均色和理论值比对，输出偏色诊断。

二维码内容 = 颜色代号 + 角标，比如红色的左上角是 "RTL"：
    颜色代号  R 红 / G 绿 / B 蓝 / W 白 / K 黑 / N 中灰
所以每张照片自带「这是哪个颜色」的信息，拍完随便排序、随便命名都能对上。

用法：
    python tools/color_calib.py 照片1.jpg 照片2.jpg ...
    python tools/color_calib.py 照片目录/
    python tools/color_calib.py --debug 校正后预览.png 照片目录/

依赖：
    pip install opencv-python numpy
    pip install pyzbar          # 可选，装了识别率更高

注意：手机相机自带白平衡/HDR/色彩增强，会给结果叠加偏差。
      测出来的绝对值不必较真，看「趋势」——是否反色、是否 R/B 互换、
      哪个通道偏强——这些结论不受小幅相机偏色影响。
"""

import argparse
import os
import sys

import cv2
import numpy as np

# ===================== 与固件对齐的常量 =====================
# 设备端 ui_calib.h：屏幕 320x240，四角白底 88x88（二维码 64 + 12 留边）。
# 改固件里的 UI_CALIB_QR_SIZE / UI_CALIB_QR_PAD 时，这几个常量要跟着改。
SCREEN_W, SCREEN_H = 320, 240
BOX = 88

# 四个定位底板中心在屏幕上的坐标（= BOX / 2）
ANCHOR_DST = {
    "TL": (44, 44),
    "TR": (276, 44),
    "BL": (44, 196),
    "BR": (276, 196),
}

# 取色区域：纯色区是 x∈[88,232] y∈[88,152]，这里再往里缩 8px
ROI = (96, 96, 224, 144)        # x0, y0, x1, y1

COLOR_ORDER = ["R", "G", "B", "W", "K", "N"]
COLOR_INFO = {
    "R": ("红",   (255, 0, 0)),
    "G": ("绿",   (0, 255, 0)),
    "B": ("蓝",   (0, 0, 255)),
    "W": ("白",   (255, 255, 255)),
    "K": ("黑",   (0, 0, 0)),
    "N": ("中灰", (128, 128, 128)),
}

SUFFIXES = ("TL", "TR", "BL", "BR")

IMAGE_EXT = (".jpg", ".jpeg", ".png", ".bmp", ".webp")


# ===================== 读图 =====================

def imread_unicode(path):
    """cv2.imread 在 Windows 下不认中文路径，绕一下"""
    data = np.fromfile(path, dtype=np.uint8)
    if data.size == 0:
        return None
    return cv2.imdecode(data, cv2.IMREAD_COLOR)


def imwrite_unicode(path, img):
    """同样绕开 cv2.imwrite 的中文路径问题"""
    ext = os.path.splitext(path)[1] or ".png"
    ok, buf = cv2.imencode(ext, img)
    if not ok:
        return False
    buf.tofile(path)
    return True


def collect_images(paths):
    out = []
    for p in paths:
        if os.path.isdir(p):
            for name in sorted(os.listdir(p)):
                if name.lower().endswith(IMAGE_EXT):
                    out.append(os.path.join(p, name))
        else:
            out.append(p)
    return out


# ===================== 定位四角二维码 =====================

def _parse_mark(text):
    """把 "RTL" 解析成 ("R", "TL")，不合法返回 None"""
    text = text.strip().upper()
    if len(text) == 3 and text[0] in COLOR_INFO and text[1:] in SUFFIXES:
        return text[0], text[1:]
    return None


def _locate_with_pyzbar(gray):
    from pyzbar import pyzbar

    found = {}
    color_code = None
    for sym in pyzbar.decode(gray):
        try:
            text = sym.data.decode("utf-8")
        except UnicodeDecodeError:
            continue
        parsed = _parse_mark(text)
        if parsed is None:
            continue
        code, suffix = parsed
        if color_code is None:
            color_code = code
        elif color_code != code:
            raise ValueError("照片里同时出现了两种颜色的二维码（%s 和 %s），"
                             "多半是切换颜色时快门没跟上，重拍一张" % (color_code, code))

        r = sym.rect
        found[suffix] = (r.left + r.width / 2.0, r.top + r.height / 2.0)

    return color_code, found


# 单独重读某个码时，裁出来的那一小块统一缩到这个长边再喂给检测器
_RETRY_SIDE = 420.0

_DETECTORS = None


def _get_detectors():
    """可用的二维码检测器。

    两个都用：实测 cv2.QRCodeDetectorAruco 对「拍屏」这种模块边缘被糊成
    波浪状的图强很多 —— 同一批照片默认检测器一个角都认不出，
    Aruco 版能在好几个尺度下凑齐四个。结果合并，谁认出来算谁的。
    老版本 OpenCV 没有 Aruco 版，动态探测一下。"""
    global _DETECTORS
    if _DETECTORS is None:
        _DETECTORS = [cv2.QRCodeDetector()]
        aruco = getattr(cv2, "QRCodeDetectorAruco", None)
        if aruco is not None:
            _DETECTORS.append(aruco())
    return _DETECTORS


def _decode_once(det, gray):
    """用一个检测器找一遍，返回 (颜色代号, {角标: 点}, [候选点])。
    坐标是传进来这张图的尺度，还原成原图尺度由调用方负责。"""
    ok, infos, points, _ = det.detectAndDecodeMulti(gray)
    if not ok or points is None or len(points) == 0:
        return None, {}, []

    found = {}
    color_code = None
    candidates = []

    for info, pts in zip(infos, points):
        pts = pts.reshape(-1, 2)
        candidates.append(pts)
        parsed = _parse_mark(info or "")
        if parsed is None:
            continue

        code, suffix = parsed
        if color_code is None:
            color_code = code
        elif color_code != code:
            raise ValueError("照片里同时出现了两种颜色的二维码（%s 和 %s），"
                             "多半是切换颜色时二维码还没刷新完，重拍一张"
                             % (color_code, code))
        found.setdefault(suffix, (float(pts[:, 0].mean()), float(pts[:, 1].mean())))

    # 四个候选点都检测到了、只是有个别没解码出来：按位置把它补上。
    # 必须拿已经解码出来的码校验 —— 万一照片被转过 180°，
    # 「离哪个角最近」的判断会全部反过来，那就宁可不用。
    if 1 <= len(found) < 4 and len(candidates) == 4:
        guess = guess_by_position(candidates)
        if guess is not None and all(
                np.allclose(guess.get(sfx, (-1e9, -1e9)), pt, atol=0.5)
                for sfx, pt in found.items()):
            for sfx, pt in guess.items():
                found.setdefault(sfx, (float(pt[0]), float(pt[1])))

    return color_code, found, candidates


def _retry_single(gray, pts):
    """候选点没解码出来，就把那块裁出来单独重读一次。
    拍屏图里四个码大小差很多，远角那个常常就差这一点点。"""
    h, w = gray.shape
    x0 = max(0, int(pts[:, 0].min()) - 6)
    y0 = max(0, int(pts[:, 1].min()) - 6)
    x1 = min(w, int(pts[:, 0].max()) + 6)
    y1 = min(h, int(pts[:, 1].max()) + 6)
    if x1 - x0 < 8 or y1 - y0 < 8:
        return None

    crop = gray[y0:y1, x0:x1]
    scale = _RETRY_SIDE / float(max(crop.shape[:2]))
    interp = cv2.INTER_AREA if scale < 1.0 else cv2.INTER_CUBIC
    big = cv2.resize(crop, None, fx=scale, fy=scale, interpolation=interp)

    for det in _get_detectors():
        code, found, _ = _decode_once(det, big)
        for suffix, p in found.items():
            return code, suffix, (p[0] / scale + x0, p[1] / scale + y0)
    return None


def guess_by_position(quads):
    """四个候选码都在、但不是个个都解码成功时，按「中心离哪个角最近」分派角标。

    quads 里每个元素是一个码的四个角点（4x2）。
    只在照片没被转过 90°/180° 时才对，所以调用方必须拿已解码的结果校验。"""
    if len(quads) != 4:
        return None

    centers = [(float(q[:, 0].mean()), float(q[:, 1].mean())) for q in quads]
    cx = sum(c[0] for c in centers) / 4.0
    cy = sum(c[1] for c in centers) / 4.0

    guess = {}
    for c in centers:
        right = c[0] > cx
        bottom = c[1] > cy
        guess[("TL", "TR", "BL", "BR")[(2 if bottom else 0) + (1 if right else 0)]] = c

    if len(guess) != 4:
        return None
    return guess


def fill_missing_anchor(found):
    """四个角只读出三个时，把缺的那个补出来。

    屏幕是矩形，拍屏时透视不重，照片里四个码中心近似构成平行四边形，
    所以「缺的角 = 相邻两角之和 - 对角」。取样区在屏幕正中，
    角点这点估计误差传不到中间，够用。
    """
    if len(found) != 3:
        return found

    missing = [s for s in SUFFIXES if s not in found]
    if len(missing) != 1:
        return found

    miss = missing[0]
    opp = {"TL": "BR", "TR": "BL", "BL": "TR", "BR": "TL"}[miss]
    if opp not in found:
        return found

    others = [s for s in SUFFIXES if s != miss and s != opp]
    x = found[others[0]][0] + found[others[1]][0] - found[opp][0]
    y = found[others[0]][1] + found[others[1]][1] - found[opp][1]
    found[miss] = (x, y)
    print("  （%s 角没读出内容，按平行四边形补出来了）" % miss)
    return found


def _multi_pass(gray, scale):
    """在一张（可能已缩放的）图上，把所有检测器各跑一遍再合并结果。
    scale 是这张图相对原图的倍数，返回的坐标统一还原成原图尺度。"""
    merged = {}
    color_code = None
    candidates = []

    for det in _get_detectors():
        code, found, cands = _decode_once(det, gray)

        if code is not None:
            if color_code is None:
                color_code = code
            elif code != color_code:
                continue            # 两个检测器认出的颜色对不上，这轮结果丢掉

        for suffix, p in found.items():
            merged.setdefault(suffix, (p[0] / scale, p[1] / scale))

        if len(cands) > len(candidates):
            candidates = [c / scale for c in cands]

    return color_code, merged, candidates


def _resize(gray, scale):
    """缩小时用 AREA —— 对屏幕拍出来的摩尔纹最不容易二次走样"""
    return cv2.resize(gray, None, fx=scale, fy=scale, interpolation=cv2.INTER_AREA)


def _scale_passes(gray):
    """按几个尺寸各扫一轮，结果合并，凑齐四个角就停。

    手机原图动辄 3000~4600 px，而 OpenCV 的二维码检测在这么大的图上反而
    经常漏检 —— 实测同一组照片，原图只有 3/6 张能认出四个角，
    缩到长边 700~1200 则 6/6 全中。所以先缩后认。

    第二轮再拿轻度模糊过的图扫一遍：手机为了去掉拍屏摩尔纹会做降噪，
    顺手把二维码模块的边缘啃成锯齿，糊一下反而干净。第二轮只在
    第一轮凑不齐时才跑，平时不白花这个开销。"""
    longest = max(gray.shape[:2])

    def sweep(src):
        for target in (700, 900, 1200, 1700, 2400):
            scale = target / float(longest)
            if scale >= 0.98:
                continue                # 原图本来就小，不做无意义的放大
            yield scale, _resize(src, scale)
        yield 1.0, src                  # 兜底再来一次原尺寸

    yield from sweep(gray)

    soft = cv2.GaussianBlur(gray, (9, 9), 0)   # 走到这里说明第一轮没成
    yield from sweep(soft)


def _locate_with_cv2(gray):
    """找四角：多个尺寸各扫一轮，中间穿插「裁出来单独重读」补漏角。

    注意别换成单码 detectAndDecode —— 实测它在「屏幕拍屏」图上完全失效，
    而 multi 接口一次能给全四个。"""
    merged = {}
    code = None

    for scale, src in _scale_passes(gray):
        c, f, candidates = _multi_pass(src, scale)

        if c is not None:
            if code is None:
                code = c
            elif c != code:
                continue                    # 这一轮认出的颜色对不上，结果丢掉

        for s, p in f.items():
            merged.setdefault(s, p)

        # 还差的角，就把那些没读出内容的候选裁出来单独重读一遍
        for pts in candidates:
            if len(merged) >= 4:
                break
            got = _retry_single(gray, pts)
            if got is None:
                continue
            c2, suffix, point = got
            if code is not None and c2 is not None and c2 != code:
                continue
            code = code if code is not None else c2
            merged.setdefault(suffix, point)

        if len(merged) >= 4:
            break

    return code, merged


def locate_anchors(gray):
    """返回 (颜色代号, {角标: (x, y)})"""
    try:
        code, found = _locate_with_pyzbar(gray)
    except ImportError:
        code, found = None, {}

    if len(found) != 4:
        # pyzbar 没装或没认全，换 OpenCV 再试一次
        code2, found2 = _locate_with_cv2(gray)
        if len(found2) > len(found):
            code, found = code2, found2

    return code, found


# ===================== 校正与取色 =====================

def rectify(img, anchors):
    src = np.float32([anchors[s] for s in SUFFIXES])
    dst = np.float32([ANCHOR_DST[s] for s in SUFFIXES])
    m = cv2.getPerspectiveTransform(src, dst)
    return cv2.warpPerspective(img, m, (SCREEN_W, SCREEN_H))


def sample_color(warped):
    x0, y0, x1, y1 = ROI
    roi = warped[y0:y1, x0:x1].reshape(-1, 3).astype(np.float32)
    b, g, r = np.median(roi, axis=0)
    return int(round(r)), int(round(g)), int(round(b))


# ===================== 诊断 =====================

def fmt_rgb(rgb):
    return "(%3d,%3d,%3d)" % rgb


def analyze(samples):
    """samples: {颜色代号: (r, g, b)}，缺失的颜色不在里面"""
    out = []
    missing = [c for c in COLOR_ORDER if c not in samples]

    out.append("")
    out.append("===== 逐色对比 =====")
    out.append("%-4s %-15s %-15s %6s %6s %6s" %
               ("颜色", "实测 RGB", "理论 RGB", "dR", "dG", "dB"))
    for c in COLOR_ORDER:
        if c not in samples:
            continue
        name, exp = COLOR_INFO[c]
        got = samples[c]
        out.append("%-4s %-15s %-15s %+6d %+6d %+6d" % (
            name, fmt_rgb(got), fmt_rgb(exp),
            got[0] - exp[0], got[1] - exp[1], got[2] - exp[2]))

    if missing:
        out.append("")
        out.append("缺少这些颜色的照片：%s" %
                   " ".join(COLOR_INFO[c][0] for c in missing))

    out.append("")
    out.append("===== 诊断 =====")

    # 先判断数据本身靠不靠谱：白块本该顶到 255，明显不到就是欠曝，
    # 这种数据算出来的通道增益没有意义，先提醒重拍而不是硬给结论。
    # 拿白块当基准而不是三原色 —— 纯色块的读数会被相机的色彩处理压低，
    # 三原色里总有一个偏高，用它做判据会被掩盖过去。
    bright = max(samples["W"]) if "W" in samples else None
    if bright is not None and bright < 230:
        out.append("注意        : 照片整体偏暗，白块最亮的通道只有 %d"
                   "（理想应接近 255）。" % bright)
        out.append("              下面的通道增益不可信，请调高手机曝光后重拍。")

    # 过曝同样让数据失真：白块顶死到 255 之后，通道强弱和灰阶都看不出来了
    if "W" in samples and min(samples["W"]) >= 250:
        out.append("注意        : 白块三个通道都在 250 以上，照片过曝了。")
        out.append("              过曝时通道增益和灰阶都测不准，请把曝光降一档重拍。")

    findings = []

    # 黑电平 / 反色：黑块本该接近 0，白块本该接近 255
    luma = {c: (0.299 * v[0] + 0.587 * v[1] + 0.114 * v[2]) for c, v in samples.items()}

    if "K" in luma:
        lk = luma["K"]
        out.append("黑电平      : 黑块实测亮度 %.0f（理想 0；偏高说明漏光、反光或偏置不对）" % lk)
        if lk > 100:
            findings.append("黑块测出来很亮")
    else:
        lk = None

    if "K" in luma and "W" in luma:
        if luma["K"] > luma["W"]:
            findings.append("黑比白还亮 → 屏幕整体反色（LCD inversion / 反相设置不对）")
    out.append("反色        : %s" % ("是" if any("反色" in f for f in findings) else "否"))

    # R/B 互换：红块测出来蓝分量更大、蓝块测出来红分量更大
    rb_swap = False
    if "R" in samples and "B" in samples:
        r_of_red = samples["R"]
        b_of_blue = samples["B"]
        if r_of_red[2] > r_of_red[0] and b_of_blue[0] > b_of_blue[2]:
            rb_swap = True
    out.append("R-B 互换    : %s" % ("是" if rb_swap else "否"))
    if rb_swap:
        findings.append("红蓝通道互换 → RGB / BGR 顺序搞反了")

    # 白平衡（以 G 为基准，只在小幅范围内才有意义）
    if "W" in samples:
        r, g, b = samples["W"]
        if g > 8:
            gr, gb = r / g, b / g
            out.append("白平衡      : R/G = %.2f  B/G = %.2f（理想都是 1.00）" % (gr, gb))
            if gr > 1.15:
                findings.append("偏红（R 通道偏强）")
            elif gr < 0.85:
                findings.append("偏青（R 通道偏弱）")
            if gb > 1.15:
                findings.append("偏蓝（B 通道偏强）")
            elif gb < 0.85:
                findings.append("偏黄（B 通道偏弱）")
        else:
            out.append("白平衡      : 白块太暗，无法判断")

    # 灰阶：中灰 / 白的亮度比应接近 128/255
    if "N" in luma and "W" in luma and luma["W"] > 40:
        ratio = luma["N"] / luma["W"]
        out.append("灰阶        : 中灰/白 = %.2f（理想 0.50，偏差大说明 gamma 不线性）" % ratio)
        if ratio > 0.62:
            findings.append("中灰偏亮（gamma 偏高 / 暗部被提亮）")
        elif ratio < 0.38:
            findings.append("中灰偏暗（gamma 偏低 / 暗部被压黑）")

    out.append("")
    out.append("===== 结论 =====")
    if findings:
        for f in findings:
            out.append("  · " + f)
        out.append("")
        out.append("  提示：以上偏差里含手机相机自身的白平衡与色彩增强，绝对值不必较真。")
        out.append("        若想剥离相机因素，可用同一部手机先拍一张白纸/灰卡对比。")
    else:
        out.append("  未发现明显异常：颜色顺序、灰度走向都正常。")
        out.append("  残余的小幅偏差多半来自手机相机本身。")

    return "\n".join(out)


# ===================== 主流程 =====================

def main():
    ap = argparse.ArgumentParser(
        description="屏幕色彩校准分析（配合固件串口命令 cal 使用）",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="+", help="照片文件或目录")
    ap.add_argument("--debug", metavar="PNG",
                    help="把透视校正后的图片拼成一张，存到该路径，便于排查取色位置")
    args = ap.parse_args()

    files = collect_images(args.paths)
    if not files:
        print("没找到图片文件")
        return 1

    samples = {}
    rectified = []

    for path in files:
        img = imread_unicode(path)
        if img is None:
            print("[跳过] 读不出图片: %s" % path)
            continue

        gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
        try:
            code, anchors = locate_anchors(gray)
        except ValueError as e:
            print("[跳过] %s: %s" % (path, e))
            continue

        if code is not None:
            anchors = fill_missing_anchor(anchors)

        if code is None or len(anchors) != 4:
            print("[跳过] %s: 只认到 %d 个二维码，需要四个角都拍到且清晰"
                  % (path, len(anchors)))
            continue

        warped = rectify(img, anchors)
        rgb = sample_color(warped)
        name = COLOR_INFO[code][0]
        print("%-28s → %s %s" % (os.path.basename(path), name, fmt_rgb(rgb)))

        samples[code] = rgb
        rectified.append(warped)

    if not samples:
        print("\n没有任何一张照片可用。")
        print("检查：四角二维码是否拍全、画面是否太暗/太糊、是否用了小图缩略图。")
        return 1

    print(analyze(samples))

    if args.debug and rectified:
        per_row = min(len(rectified), 3)
        rows = (len(rectified) + per_row - 1) // per_row
        sheet = np.zeros((rows * SCREEN_H, per_row * SCREEN_W, 3), np.uint8)
        for i, w in enumerate(rectified):
            r, c = divmod(i, per_row)
            sheet[r * SCREEN_H:(r + 1) * SCREEN_H,
                  c * SCREEN_W:(c + 1) * SCREEN_W] = w
        # 画上取色框，确认框在色块里而不是压在二维码上
        x0, y0, x1, y1 = ROI
        for i in range(len(rectified)):
            r, c = divmod(i, per_row)
            cv2.rectangle(sheet, (c * SCREEN_W + x0, r * SCREEN_H + y0),
                          (c * SCREEN_W + x1, r * SCREEN_H + y1), (0, 255, 255), 1)
        imwrite_unicode(args.debug, sheet)
        print("\n校正预览已保存: %s（黄框是取色区域）" % args.debug)

    return 0


if __name__ == "__main__":
    sys.exit(main())
