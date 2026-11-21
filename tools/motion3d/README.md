# 姿态 3D 显示（陀螺仪 → 电脑）

设备端：`设置 → 关于 → 姿态广播` 打开后，约 30Hz 通过 UDP 4210 端口广播一帧：

```
MOTION <seq> <t_ms> <qw> <qx> <qy> <qz> <ax> <ay> <az> <gx> <gy> <gz>
四元数 ×10000，加速度 ×1000（g），角速度 ×100（°/s）
```

电脑端运行本目录的 `motion_view.py`，同一个 WiFi 下就能看到外壳模型跟着设备转。
**只显示姿态**：加速度积分出来的位移漂移严重，已经去掉（协议里的 ax/ay/az 仍保留，
方便以后做别的用途）。

## 运行

```powershell
python -m pip install PyQt5 numpy
python tools\motion3d\motion_view.py
```

或双击 `run.bat`。

## 操作

| 按键 | 作用 |
|---|---|
| G | 姿态归零（把当前朝向往成初始朝向） |
| X / Y / Z | 模型绕对应轴旋转 90°（对齐芯片焊接方向） |

## 真实外壳模型（可选）

默认是简化外壳（盒子 + 屏幕 + 底座）。要用 STEP 外壳：

```powershell
python -m pip install cascadio trimesh
python tools\motion3d\convert_step.py "D:\#MORUA\3D\模型\微雪-ESP32-C5-LCD2.8Touch_磁吸底座阅读器外壳.step"
```

转换得到 `model.obj` 后，`motion_view.py` 会自动改用真实模型。73MB 的 STEP
转换较慢（几分钟、几个 GB 内存），且外壳倒角多；面数超过 2 万会被拒绝加载
（软件渲染会卡）。建议先在 CAD 里删掉内部零件、只留外壳，或用 `--decimate`
控制面数。

## 坐标说明

- 固件发的四元数/加速度就是 QMI8658 的芯片轴，没有做屏幕方向的翻转；
- 电脑端模型坐标是 X 右、Y 前、Z 上；
- 如果发现模型初始朝向不对，按 X/Y/Z 让模型转 90° 对齐，再按 G 归零即可。
- 位移是加速度积分的估算值：静止时会有轻微漂移，按 R 归零。
