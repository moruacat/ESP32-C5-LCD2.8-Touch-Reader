#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
photo2raw 图形界面 —— 把图片批量转成 320x240 RGB565 小端裸数据(.raw)

运行：
    python tools\\photo2raw\\photo2raw_gui.py
    （把后缀改成 .pyw 再用 pythonw 启动就没有黑色控制台窗口）

界面上的实用功能：
    - 选图片文件（可多选）/ 选文件夹（批量，含子目录）
    - 输出目录（留空 = 与源文件同目录）
    - 覆盖已存在的 .raw 开关
    - 开始 / 停止（停止会中断后续还没转的文件）
    - 进度条 + 实时日志

转换规格：等比缩放到不超过 320x240（只缩小不放大），居中贴到黑底画布，
输出纯像素（无文件头）的 320x240 RGB565 小端数据，固定 153600 字节。
生成的 .raw 拷进 SD 卡的 /photos 目录即可。

转换逻辑全部在 photo2raw.py 里，这个文件只管界面。
"""

from __future__ import annotations

import queue
import sys
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

sys.path.insert(0, str(Path(__file__).resolve().parent))
import photo2raw                                           # noqa: E402

UI_FONT = ("Microsoft YaHei UI", 9)
PAD = 8
MULTI_SEP = "; "            # 多选文件在输入框里的分隔符


class ToolTip:
    """鼠标悬停提示"""

    def __init__(self, widget: tk.Widget, text: str):
        self.widget = widget
        self.text = text
        self.tip: tk.Toplevel | None = None
        widget.bind("<Enter>", self._show, add="+")
        widget.bind("<Leave>", self._hide, add="+")
        widget.bind("<ButtonPress>", self._hide, add="+")

    def _show(self, _event=None) -> None:
        if self.tip is not None:
            return
        x = self.widget.winfo_rootx() + 16
        y = self.widget.winfo_rooty() + self.widget.winfo_height() + 4
        self.tip = tk.Toplevel(self.widget)
        self.tip.wm_overrideredirect(True)
        self.tip.wm_geometry(f"+{x}+{y}")
        tk.Label(self.tip, text=self.text, justify="left", background="#ffffe0",
                 relief="solid", borderwidth=1, font=UI_FONT, padx=6, pady=3).pack()

    def _hide(self, _event=None) -> None:
        if self.tip is not None:
            self.tip.destroy()
            self.tip = None


class App:
    def __init__(self, root: tk.Tk):
        self.root = root
        root.title("图片 → RGB565 RAW 转换工具")
        root.geometry("660x620")
        root.minsize(580, 500)
        root.option_add("*Font", UI_FONT)

        self.in_path = tk.StringVar()
        self.out_dir = tk.StringVar()
        self.opt_overwrite = tk.BooleanVar(value=False)
        self.status = tk.StringVar(value="就绪")

        self.queue: queue.Queue = queue.Queue()
        self.busy = False
        self.stop_flag = threading.Event()

        self._build()
        self.root.after(80, self._drain)

    # ---------- 界面 ----------

    def _build(self) -> None:
        outer = ttk.Frame(self.root, padding=PAD)
        outer.pack(fill="both", expand=True)
        outer.columnconfigure(1, weight=1)
        outer.rowconfigure(6, weight=1)

        # 输入
        ttk.Label(outer, text="图片文件 / 目录:").grid(row=0, column=0, sticky="w", pady=4)
        ttk.Entry(outer, textvariable=self.in_path).grid(row=0, column=1, sticky="ew",
                                                        padx=4, pady=4)
        btn_file = ttk.Button(outer, text="选图片文件", width=11, command=self.pick_files)
        btn_file.grid(row=0, column=2, pady=4)
        btn_dir = ttk.Button(outer, text="选文件夹", width=9, command=self.pick_dir)
        btn_dir.grid(row=0, column=3, padx=(4, 0), pady=4)
        ToolTip(btn_file, "选择一张或多张图片（PNG / JPG / JPEG / WEBP）\n"
                          f"多选时在输入框里用 \"{MULTI_SEP.strip()}\" 分隔")
        ToolTip(btn_dir, "选择文件夹，批量转换里面所有的图片（含子目录）")

        # 输出
        ttk.Label(outer, text="输出目录:").grid(row=1, column=0, sticky="w", pady=4)
        ttk.Entry(outer, textvariable=self.out_dir).grid(row=1, column=1, sticky="ew",
                                                         padx=4, pady=4)
        btn_out = ttk.Button(outer, text="浏览", width=8, command=self.pick_out)
        btn_out.grid(row=1, column=2, pady=4)
        btn_src = ttk.Button(outer, text="用源目录", width=9, command=self.use_src_dir)
        btn_src.grid(row=1, column=3, padx=(4, 0), pady=4)
        ToolTip(btn_out, "生成的 .raw 放在哪里\n留空 = 放在源图片旁边")
        ToolTip(btn_src, "清空输出目录，改成与源图片同目录")

        ttk.Label(outer, text="留空则与源图片同目录；输出 320×240 RGB565 小端 .raw"
                              "（153600 字节），拷进 SD 卡 /photos 目录即可",
                  foreground="#777777", wraplength=620, justify="left").grid(
            row=2, column=0, columnspan=4, sticky="w", pady=(0, 6))

        # 选项
        opts = ttk.LabelFrame(outer, text="选项", padding=PAD)
        opts.grid(row=3, column=0, columnspan=4, sticky="ew", pady=4)

        cb_over = ttk.Checkbutton(opts, text="覆盖已存在的 .raw",
                                  variable=self.opt_overwrite)
        cb_over.grid(row=0, column=0, sticky="w")
        ToolTip(cb_over, "不勾选时，同名输出已存在就自动加序号 name_1.raw、name_2.raw …\n"
                         "勾选后直接覆盖同名文件")

        # 进度
        self.progress = ttk.Progressbar(outer, mode="determinate", maximum=100)
        self.progress.grid(row=4, column=0, columnspan=4, sticky="ew", pady=(10, 2))

        # 开始 / 停止
        run_row = ttk.Frame(outer)
        run_row.grid(row=5, column=0, columnspan=4, pady=8)
        self.btn_start = ttk.Button(run_row, text="开始转换", width=16, command=self.start)
        self.btn_start.pack(side="left", padx=4)
        self.btn_stop = ttk.Button(run_row, text="停止", width=10, command=self.stop,
                                   state="disabled")
        self.btn_stop.pack(side="left", padx=4)

        # 日志
        log_box = ttk.LabelFrame(outer, text="日志", padding=4)
        log_box.grid(row=6, column=0, columnspan=4, sticky="nsew")
        log_box.columnconfigure(0, weight=1)
        log_box.rowconfigure(0, weight=1)

        self.log_text = tk.Text(log_box, wrap="word", height=12, relief="flat",
                                background="#fbfbfb", state="normal")
        self.log_text.grid(row=0, column=0, sticky="nsew")
        self.log_text.bind("<Key>", self._readonly_key)

        bar = ttk.Scrollbar(log_box, command=self.log_text.yview)
        bar.grid(row=0, column=1, sticky="ns")
        self.log_text.config(yscrollcommand=bar.set)

        self.log_text.tag_config("info", foreground="#222222")
        self.log_text.tag_config("warn", foreground="#b06000")
        self.log_text.tag_config("error", foreground="#c00000")
        self.log_text.tag_config("ok", foreground="#0a7a0a")

        ttk.Label(outer, textvariable=self.status, foreground="#555555").grid(
            row=7, column=0, columnspan=4, sticky="w", pady=(6, 0))

    def _readonly_key(self, event):
        """日志区只读，但放行 Ctrl+C / Ctrl+A 这类组合键"""
        if event.state & 0x4:
            return None
        return "break"

    # ---------- 事件 ----------

    def pick_files(self) -> None:
        names = filedialog.askopenfilenames(
            title="选择图片（可多选）",
            filetypes=[("图片", "*.png *.jpg *.jpeg *.webp"),
                       ("PNG", "*.png"), ("JPEG", "*.jpg *.jpeg"),
                       ("WEBP", "*.webp"), ("所有文件", "*.*")])
        if names:
            self.in_path.set(MULTI_SEP.join(names))

    def pick_dir(self) -> None:
        name = filedialog.askdirectory(title="选择包含图片的文件夹")
        if name:
            self.in_path.set(name)

    def pick_out(self) -> None:
        name = filedialog.askdirectory(title="选择输出目录")
        if name:
            self.out_dir.set(name)

    def use_src_dir(self) -> None:
        self.out_dir.set("")

    def stop(self) -> None:
        if self.busy:
            self.stop_flag.set()
            self.btn_stop.config(state="disabled")
            self.status.set("正在停止…（当前文件转完后停下）")

    def start(self) -> None:
        if self.busy:
            return

        raw = self.in_path.get().strip()
        if not raw:
            messagebox.showerror("错误", "请先选择图片文件或包含图片的文件夹")
            return

        paths = [p.strip() for p in raw.split(";") if p.strip()]
        out = self.out_dir.get().strip()
        out_root: Path | None = Path(out) if out else None

        # 先清空日志，再收集输入（收集时的 [跳过] 提示会排进队列、随后显示）
        self.log_text.delete("1.0", "end")

        opt = photo2raw.Options(
            out_root=out_root,
            overwrite=self.opt_overwrite.get(),
            log=lambda m: self.queue.put(("log", m)),
            warn=lambda m: self.queue.put(("warn", m)),
        )
        files, skipped = photo2raw.collect_inputs(paths, opt.warning)
        if not files:
            messagebox.showerror("错误", "没有找到可转换的图片")
            self.status.set("没有可转换的图片")
            return

        if out_root is not None:
            out_root.mkdir(parents=True, exist_ok=True)

        self.busy = True
        self.stop_flag.clear()
        self.btn_start.config(state="disabled")
        self.btn_stop.config(state="normal")
        self.progress["value"] = 0
        self.status.set(f"开始转换…共 {len(files)} 个文件")

        threading.Thread(target=self._work, args=(files, opt, skipped), daemon=True).start()

    def _work(self, files: list[Path], opt: photo2raw.Options, skipped: int) -> None:
        """后台线程：只往队列里丢消息，不碰任何控件"""
        ok = fail = 0
        total = len(files)
        stopped = False
        for i, f in enumerate(files):
            if self.stop_flag.is_set():
                stopped = True
                break
            if total > 1:
                self.queue.put(("log", f"\n===== [{i + 1}/{total}] {f.name} ====="))
            else:
                self.queue.put(("log", f"[转换] {f.name}"))
            st = photo2raw.convert_one(f, opt)
            if st == photo2raw.STATUS_OK:
                ok += 1
            else:
                fail += 1
            self.queue.put(("progress", (i + 1, total, f.name)))
        self.queue.put(("done", (ok, skipped, fail, total, stopped)))

    # ---------- 主线程消息泵 ----------

    def _drain(self) -> None:
        try:
            while True:
                kind, payload = self.queue.get_nowait()
                if kind == "log":
                    self._append(payload, "info")
                elif kind == "warn":
                    self._append(payload, "error" if payload.startswith("[失败]") else "warn")
                elif kind == "progress":
                    done, total, msg = payload
                    self.progress["value"] = (done / total * 100) if total else 0
                    if msg:
                        self.status.set(f"转换中 {done}/{total}：{msg}")
                elif kind == "done":
                    self._finish(*payload)
        except queue.Empty:
            pass
        self.root.after(80, self._drain)

    def _append(self, msg: str, tag: str) -> None:
        self.log_text.insert("end", msg + "\n", tag)
        self.log_text.see("end")

    def _finish(self, ok: int, skipped: int, fail: int, total: int, stopped: bool) -> None:
        self.busy = False
        self.btn_start.config(state="normal")
        self.btn_stop.config(state="disabled")
        if not stopped:
            self.progress["value"] = 100

        summary = f"成功 {ok}，跳过 {skipped}，失败 {fail}"
        if stopped:
            summary = f"已停止（已处理 {ok + fail}/{total}）。" + summary
            self.status.set(summary)
            self._append(summary, "warn")
            messagebox.showwarning("已停止", summary)
        elif fail:
            self.status.set("完成：" + summary)
            self._append(summary, "warn")
            messagebox.showwarning("完成（有失败）", "转换完成\n" + summary + "\n详情见日志。")
        else:
            self.status.set("转换完成：" + summary)
            self._append("转换完成。" + summary, "ok")
            messagebox.showinfo("完成",
                                "转换完成\n" + summary +
                                "\n把生成的 .raw 拷到 SD 卡的 /photos 目录即可。")


def main() -> None:
    try:                                   # 高 DPI 屏上别糊（必须在建窗口之前设）
        from ctypes import windll
        windll.shcore.SetProcessDpiAwareness(1)
    except Exception:
        pass

    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
