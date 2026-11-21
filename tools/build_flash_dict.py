#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成放进 Flash 分区的离线词库（`flash_assets/dict/`）。

完整词库 dict/dict.dat 有 17MB（15.7 万条），32MB Flash 扣掉 app/OTA 之后
放不下；而 SD 卡又不是一直都在。所以这里从现有主表里裁一份「常用词」：

  1. 有考试/牛津/Collins 标签的词（构建脚本认为「常用」的那批）
  2. 人工释义表里的词
  3. 其余短词（纯字母、长度 <= 6）按原顺序补满预算

产物格式和 dict/dict.dat、dict.idx 完全一致（5 字段 / 每 64 行一个锚点），
设备端不需要第二套解析代码。SD 卡上有完整词库时优先用 SD 的。

用法：
    python tools/build_flash_dict.py                 # 裁剪词库 + 生成 build/dict.bin
    python tools/build_flash_dict.py --no-image      # 只生成数据（不生成 SPIFFS 镜像）
    python tools/build_flash_dict.py --budget 3.0    # 更小的裁剪
    python tools/build_flash_dict.py --image D:/x.bin

说明：
    dict.bin 是 dict 分区（9MB SPIFFS）的完整镜像，CI 面板烧录时会自动带上它
    （见 tools/ci_panel）。找不到 IDF 的 spiffsgen.py 时会提示手动路径。
"""

import argparse
import io
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _manual_defs as MD          # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DAT = os.path.join(ROOT, "dict", "dict.dat")
OUT_DIR = os.path.join(ROOT, "flash_assets", "dict")
OUT_DAT = os.path.join(OUT_DIR, "dict.dat")
OUT_IDX = os.path.join(OUT_DIR, "dict.idx")
DEFAULT_IMAGE = os.path.join(ROOT, "build", "dict.bin")
DEFAULT_PARTITION_SIZE = 0x900000   # 与 partitions.csv 的 dict 分区一致

IDF_CANDIDATES = (
    r"D:\Dev-Kit\ESP-IDF\v6.1\esp-idf",
    r"C:\Espressif\frameworks\esp-idf-v6.1",
    r"C:\Espressif\esp-idf",
)

STEP = 64
KEY_BYTES = 8
#: 考试标签 + 牛津 + Collins（flags 是单字符标签串）
KEEP_FLAGS = set("46KGZTIR") | {"O", "C"}

DEFAULT_BUDGET_MB = 7.5


def find_spiffsgen():
    """按 IDF_PATH / 常见安装路径找 spiffsgen.py"""
    env = os.environ.get("IDF_PATH")
    roots = ([env] if env else []) + list(IDF_CANDIDATES)
    for root in roots:
        if not root:
            continue
        path = os.path.join(root, "components", "spiffs", "spiffsgen.py")
        if os.path.exists(path):
            return path
    return None


def make_image(out_image, partition_size):
    """把 flash_assets/ 打成 SPIFFS 镜像（dict 分区内容）"""
    spiffsgen = find_spiffsgen()
    if spiffsgen is None:
        print("找不到 IDF 的 spiffsgen.py；只生成数据，不生成镜像。")
        print("可手动执行：python <IDF>/components/spiffs/spiffsgen.py %d flash_assets %s"
              % (partition_size, out_image))
        return False

    os.makedirs(os.path.dirname(os.path.abspath(out_image)), exist_ok=True)
    cmd = [sys.executable, spiffsgen, str(partition_size),
           os.path.join(ROOT, "flash_assets"), out_image]
    print("生成 SPIFFS 镜像：%d 字节分区 -> %s" % (partition_size, out_image))
    ret = subprocess.run(cmd)
    if ret.returncode != 0:
        print("spiffsgen.py 失败（退出码 %d）" % ret.returncode)
        return False
    print("  -> %s（%.2f MB）" % (out_image, os.path.getsize(out_image) / 1048576.0))
    return True


def tag_score(flags):
    """常用度：考试标签 / 牛津 / Collins 星数，越常用分越高（只用于诊断输出）"""
    n = sum(1 for ch in flags if ch in KEEP_FLAGS)
    for i, ch in enumerate(flags):
        if ch == "C" and i + 1 < len(flags) and flags[i + 1].isdigit():
            n += int(flags[i + 1])
    return n


def main():
    ap = argparse.ArgumentParser(description="从 dict.dat 裁剪 Flash 内置词库")
    ap.add_argument("--budget", type=float, default=DEFAULT_BUDGET_MB,
                    help="dict.dat 预算（MB），默认 %.1f" % DEFAULT_BUDGET_MB)
    ap.add_argument("--image", default=DEFAULT_IMAGE,
                    help="SPIFFS 镜像输出路径（默认 %s）" % os.path.relpath(DEFAULT_IMAGE, ROOT))
    ap.add_argument("--no-image", action="store_true",
                    help="只生成 flash_assets/ 数据，不生成 dict.bin")
    ap.add_argument("--partition-size", type=lambda s: int(s, 0),
                    default=DEFAULT_PARTITION_SIZE,
                    help="dict 分区大小（字节，默认 0x900000，与 partitions.csv 一致）")
    args = ap.parse_args()

    if not os.path.exists(SRC_DAT):
        print("找不到 %s，先跑 tools/build_dict.py" % SRC_DAT)
        return 1

    budget = int(args.budget * 1024 * 1024)
    print("读取 %s …" % os.path.relpath(SRC_DAT, ROOT))

    must = []       # (line_bytes, score)
    short = []
    total = 0
    with io.open(SRC_DAT, "rb") as f:
        for raw in f:
            line = raw.rstrip(b"\n")
            total += 1
            parts = line.split(b"\t")
            if len(parts) < 5:
                continue
            word = parts[0].decode("utf-8", "ignore")
            flags = parts[3].decode("ascii", "ignore")
            if word in MD.MANUAL or any(ch in KEEP_FLAGS for ch in flags):
                must.append((line, tag_score(flags)))
            elif (" " not in word and "-" not in word and word.isalpha()
                  and len(word) <= 6):
                short.append((line, 0))

    print("主表 %d 行；有标签 %d 行；短词候选 %d 行" % (total, len(must), len(short)))

    # 先保证有标签的词（按常用度从高到低），再看预算补短词
    must.sort(key=lambda it: (-it[1], it[0]))
    chosen = []
    size = 0
    for line, score in must:
        if size + len(line) + 1 > budget:
            break
        chosen.append(line)
        size += len(line) + 1
    print("标签词收入 %d 行 / %.2f MB" % (len(chosen), size / 1048576.0))

    short.sort(key=lambda it: it[0])
    added_short = 0
    for line, _ in short:
        if size + len(line) + 1 > budget:
            break
        chosen.append(line)
        size += len(line) + 1
        added_short += 1
    print("短词补入 %d 行" % added_short)

    # 主表要求按 UTF-8 字节序升序（索引和二分查找都依赖这一点）
    chosen.sort()

    os.makedirs(OUT_DIR, exist_ok=True)
    offsets = []
    blob = bytearray()
    for i, line in enumerate(chosen):
        if i % STEP == 0:
            word = line.split(b"\t", 1)[0]
            offsets.append((len(blob), word[:KEY_BYTES]))
        blob += line + b"\n"

    with open(OUT_DAT, "wb") as f:
        f.write(blob)
    with open(OUT_IDX, "wb") as f:
        f.write(struct.pack("<4sHHII8s", b"DCTX", 1, STEP, len(chosen), len(blob), b"\0" * 8))
        for off, key in offsets:
            f.write(struct.pack("<I", off))
            f.write(key + b"\0" * (KEY_BYTES - len(key)))

    print("完成：%d 条，dict.dat %.2f MB，dict.idx %d KB"
          % (len(chosen), len(blob) / 1048576.0, (24 + 12 * len(offsets)) / 1024.0))
    print("产物目录：%s" % os.path.relpath(OUT_DIR, ROOT))
    print("（variants.dat 未打包；查不到词形变化时会只做规则还原）")

    if not args.no_image:
        ok = make_image(args.image, args.partition_size)
        if ok:
            print("CI 面板烧录时会自动检测这个镜像并按 0x1220000 写入 dict 分区。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
