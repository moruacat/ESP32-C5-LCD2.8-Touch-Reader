#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 STEP 外壳模型转换成 motion_view.py 能读的 model.obj（可选步骤）。

STEP 是 CAD 实体格式，PyQt 不能直接渲染，需要先三角化成网格。这里用
OpenCascade（cascadio 的 wheel）转 GLB，再用 trimesh 转 OBJ。

73MB 的 STEP 转换会比较慢（几分钟、吃几个 GB 内存），而且外壳模型通常
包含大量倒角/圆角，转出来的面数很高。motion_view.py 的面数上限是 2 万，
超了会拒绝加载（软件渲染会卡）。所以建议：

  1. 先在 FreeCAD / Fusion 里删掉内部零件、只留外壳；
  2. 或用 --decimate 让 trimesh 做一次简化（默认开启）。

安装：
    python -m pip install cascadio trimesh

用法：
    python tools/motion3d/convert_step.py "D:\\...\\外壳.step"
    python tools/motion3d/convert_step.py "..." --decimate 0.02
"""

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    ap = argparse.ArgumentParser(description="STEP -> model.obj（motion3d 用）")
    ap.add_argument("step", help="STEP 文件路径")
    ap.add_argument("-o", "--out", default=os.path.join(HERE, "model.obj"),
                    help="输出 OBJ 路径（默认 tools/motion3d/model.obj）")
    ap.add_argument("--decimate", type=float, default=0.02,
                    help="网格简化比例（0 = 不简化；默认 0.02 控制到约 2 万面内）")
    args = ap.parse_args()

    if not os.path.exists(args.step):
        print("找不到文件：%s" % args.step)
        return 1

    try:
        import cascadio
    except ImportError:
        print("缺少 cascadio。先安装：python -m pip install cascadio trimesh")
        return 1
    try:
        import trimesh
    except ImportError:
        print("缺少 trimesh。先安装：python -m pip install trimesh")
        return 1

    glb = os.path.splitext(args.out)[0] + ".glb"
    print("STEP -> GLB：%s" % args.step)
    cascadio.step_to_glb(args.step, glb)
    print("GLB：%s（%.1f MB）" % (glb, os.path.getsize(glb) / 1048576.0))

    print("加载网格 …")
    scene = trimesh.load(glb, force="mesh")
    print("三角面：%d" % len(scene.faces))

    if args.decimate > 0 and len(scene.faces) > 20000:
        target = max(4000, int(len(scene.faces) * args.decimate))
        print("简化到约 %d 面 …" % target)
        try:
            scene = scene.simplify_quadric_decimation(target)
        except Exception as e:
            print("简化失败（%s），保留原网格" % e)

    scene.export(args.out)
    print("完成：%s（%d 面）" % (args.out, len(scene.faces)))
    print("把文件命名为 model.obj 放到 %s 即可被 motion_view.py 自动加载。" % HERE)
    return 0


if __name__ == "__main__":
    sys.exit(main())
