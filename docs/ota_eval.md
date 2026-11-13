# OTA 升级方案评估（局域网 + mDNS）

> 状态：**已落地**。设备端服务在 `main/services/srv_ota.c`，CI 工具在
> `tools/ci_panel/app/discovery.py` + `OtaPushWorker`；
> 2026-11-13 首次局域网 OTA 成功（固件 `v1.5.1`，分区 factory → ota_0）。
> 结论：**可行**，Flash 空间充足；分区改造已完成（factory + ota_0，总占用 21.12 MB）。
>
> 尚待补齐（阶段 3）：OTA token 认证、设备端进度界面、失败回滚日志。

---

## 1. 现状盘点

| 项目 | 现状 | 影响 |
|---|---|---|
| Flash | **32 MB**（`CONFIG_ESPTOOLPY_FLASHSIZE_32MB`） | 空间充裕 |
| 分区占用 | 0x0000 ~ 0xF20000，约 **15.1 MB** | 剩余约 16.9 MB |
| app 分区 | 只有 `factory`（0x20000，6 MB） | **缺少 `ota_0`，当前无法 OTA** |
| `otadata` | 已存在（0xf000，8 KB） | 已具备双分区启动记录能力 |
| 固件体积 | `clock_reader.bin` **4.33 MB** | 6 MB 分区余量 1.67 MB |
| CI 产物 | bootloader / partition-table / ota_data_initial / clock_reader.bin | 缺 **OTA 专用镜像**（可复用 `clock_reader.bin`） |
| mDNS | 无 | 需要引入 `mdns` 组件 |
| HTTP server | 无（sdkconfig 有 `CONFIG_HTTPD_*` 但未跑服务） | 需要引入 `esp_http_server` |
| 设备端服务层 | `srv_wifi` / `srv_ntp` 已有网络基础 | 可复用其连接状态与事件 |

当前分区表：

```
nvs      0x009000  0x6000
otadata  0x00F000  0x2000     ← 有，但没有可切换的 app 分区
phy_init 0x011000  0x1000
factory  0x020000  0x600000   ← 唯一 app
fonts    0x620000  0x400000
storage  0xA20000  0x500000
```

---

## 2. 方案选型

### 2.1 推送式（设备跑 HTTP server，电脑 POST 固件）★ 推荐

```
CI 工具 ──mDNS 发现──> clockreader.local:8080
        ──HTTP POST /ota──> 设备 esp_ota_write → 重启
```

- **优点**：设备不需要知道电脑地址；CI 工具主动推送，与"CI 工具先接入"的目标一致；失败时设备侧可直接拒绝并返回错误。
- **缺点**：设备常驻 HTTP 服务（约 20 KB RAM + 一个任务）；局域网内无认证。
- **依赖**：`esp_http_server`（IDF 内置）+ `mdns`（IDF 内置）+ `app_update`（IDf 内置）。

### 2.2 拉取式（设备从 URL 下载）

```
设备 ──mDNS 发现电脑──> 电脑的 HTTP 服务
     ──esp_https_ota(url)──> 下载写入
```

- **优点**：标准 `esp_https_ota`，支持 HTTPS 与断点校验。
- **缺点**：设备要能解析电脑地址；需要电脑侧常驻固件服务；与"CI 工具推送"的方向相反。
- **结论**：作为**备选**，适合以后做"设备主动检查更新"。

### 2.3 结论

**选 2.1 推送式**。设备端常驻一个极小的 OTA 服务，CI 工具通过 mDNS 发现后直接 POST；后续若要做自动更新，再补 2.2。

---

## 3. 分区表改造

### 3.1 方案：`factory` + `ota_0` + `ota_1`（已落地）

```
nvs      0x009000  0x6000
otadata  0x00F000  0x2000
phy_init 0x011000  0x1000
factory  0x020000  0x600000   6 MB   ← 串口烧录 / 回滚保底
ota_0    0x620000  0x600000   6 MB   ← OTA 目标（第一次从 factory 进来）
ota_1    0xC20000  0x600000   6 MB   ← OTA 目标（之后轮换）
fonts    0x1220000 0x400000   4 MB
storage  0x1620000 0x500000   5 MB
```

- 总占用 0x1B20000 ≈ **28.4 MB ≤ 32 MB**。
- 启动路径：`factory` →（OTA）→ `ota_0` ⇄ `ota_1` 轮换。

### 3.2 为什么必须有 `ota_1`（踩过的坑）

**`esp_ota_get_next_update_partition()` 只在 `ota_0` / `ota_1` 之间找目标，`factory` 不参与轮换。**

只做 `factory + ota_0` 时：

| 当前运行分区 | `get_next_update_partition()` | 结果 |
|---|---|---|
| `factory` | `ota_0` | 第一次 OTA 正常 |
| `ota_0` | 循环回 `ota_0` 自己 | `esp_ota_begin` 报 **`ESP_ERR_OTA_PARTITION_CONFLICT`** |

也就是说**只能 OTA 一次**，第二次开始必然失败。设备端现在会提前拦下这种情况并打日志说明原因，而不是只抛一个错误码。

### 3.3 风险：分区偏移变化

`fonts` / `storage` 的偏移在加分区时会后移，**改分区表后必须完整重烧一次固件**（配置、SPIFFS 内容都会丢）。

- 缓解：把这类改造和正式发布合并，作为"一次性迁移"。
- 之后 OTA 只改 `ota_0` / `ota_1`，**不再动 `fonts` / `storage`**。

### 3.4 固件增长余量

6 MB 分区，当前固件 4.33 MB，余量 1.67 MB（38%）。LVGL + 字体 + 词典已经占大头；如果将来固件超过 5.5 MB，需要重新评估（可把 `fonts` 挪到 `storage` 统一管理，或改用压缩镜像）。

---

## 4. 设备端设计

### 4.1 mDNS

```c
mdns_init();
mdns_hostname_set("clockreader");
mdns_instance_name_set("ClockReader");
mdns_service_add("ClockReader OTA", "_clockreader-ota", "_tcp", 8080, NULL);
```

CI 工具扫描 `_clockreader-ota._tcp.local.` 拿 `host` + `port` + TXT（版本号、分区状态）。

### 4.2 HTTP 服务

| 方法 | 路径 | 说明 |
|---|---|---|
| `GET` | `/api/info` | 返回 JSON：version、sha、free_heap、boot_partition |
| `POST` | `/ota` | 请求体为固件二进制流，流式写入 `esp_ota_write` |

接收流程：

```c
esp_ota_handle_t h;
esp_ota_begin(update_part, OTA_SIZE_UNKNOWN, &h);
while ((n = httpd_req_recv(req, buf, sizeof(buf))) > 0) {
    esp_ota_write(h, buf, n);
    httpd_resp_send_chunk(req, ...);   /* 可选：回进度 */
}
esp_ota_end(h);
esp_ota_set_boot_partition(update_part);
esp_restart();
```

要点：

- **流式写入**，不整包缓冲（固件 4.33 MB，PSRAM 够但没必要）。
- 写入前 `esp_ota_get_next_update_partition(NULL)` 选目标分区。
- 失败时 `esp_ota_abort()`，不影响当前运行分区。
- 成功后 `esp_ota_set_boot_partition()` 再 `esp_restart()`。
- **首次启动**（新分区）调用 `esp_ota_mark_app_valid_cancel_rollback()`，否则会回滚。
- 服务只在**已连 WiFi 且 IP 就绪**时启动；休眠/低电时可关闭监听。

### 4.3 UI

- 设置页加"系统更新"入口：显示当前版本、目标版本、进度条。
- OTA 期间**禁用触摸与页面切换**（避免 LVGL 与写入竞争），只留一个进度界面。
- 完成提示"重启中"，失败提示错误码。

### 4.4 内存与任务

- HTTP server 单任务（栈 8 KB），`httpd_config_t.max_open_sockets = 2`。
- 接收缓冲 4 KB（内部 RAM），写入按扇区对齐。

---

## 5. CI 工具接入（本次先做）

### 5.1 发现

- 新增 `app/discovery.py`，依赖 `zeroconf`（`pip install zeroconf`）。
- 扫描 `_clockreader-ota._tcp.local.`，返回 `(name, host, port, version)`。
- **纯局域网**，不做外网穿透；找不到设备时明确提示"设备未开机或不在同一网段"。

### 5.2 推送

- 新增 `OtaWorker`：`POST http://<host>:<port>/ota`，body 为 `clock_reader.bin`。
- 分块发送（每块 64 KB），UI 显示百分比；超时 120 秒。
- 推送前先 `GET /api/info` 校验设备在线、分区可 OTA（避免把固件推给不兼容版本）。

### 5.3 UI

- 动作区新增"**OTA 推送**"按钮 + 设备下拉框 + "刷新设备"。
- 按钮在**没有成功构建的固件**时禁用（复用 `download_btn` 的状态判断）。
- 推送中禁用其它动作，结束后刷新列表。

### 5.4 依赖

```
zeroconf>=0.132
```

> 如果不想引入第三方库，备选方案是让设备把 `_clockreader-ota._tcp` 的 SRV/TXT 写进一个固定 UDP 广播（自实现），但代码量与可维护性都更差，不推荐。

---

## 6. 风险与缓解

| 风险 | 影响 | 缓解 |
|---|---|---|
| 分区表变更 | 需完整重烧，配置/SPIFFS 丢失 | 与首次发布合并；文档写明 |
| OTA 中途断电 | 新分区写入不完整 | `otadata` + 回滚：启动失败自动回 `factory` |
| 固件超 6 MB | 无法 OTA | 监控构建体积，超 5.5 MB 告警 |
| 局域网无认证 | 同网段可推任意固件 | 加 `X-OTA-Token`（配置页生成）；首次配网时写入 |
| WiFi 未连 | CI 工具发现不到 | 界面提示"先连接 WiFi" |
| 写入时 UI 卡顿 | 触摸/动画掉帧 | OTA 期间锁定 UI，只显示进度 |
| 版本不一致 | 推了旧固件 | `GET /api/info` 对比 `git describe` / 构建号 |

---

## 7. 工作量估算

| 阶段 | 内容 | 估时 |
|---|---|---|
| 0 | 分区表改造 + 完整重烧验证 | 0.5 天 |
| 1 | 设备端 mDNS + HTTP OTA 服务 | 1 天 |
| 2 | CI 工具发现 + 推送 + UI | 0.5 天 |
| 3 | 联调、断电回滚、异常路径 | 1 天 |
| 合计 | | **约 3 天** |

---

## 8. 建议路线

1. **阶段 0（本次先做 CI 工具）**：CI 工具加入 mDNS 发现与 OTA 推送界面，设备端未就绪时给出清晰报错。
2. **阶段 1**：分区表改造 + 设备端 HTTP OTA（先用 `curl` 验证）。
3. **阶段 2**：CI 工具联调，走通"构建 → 下载 → 推送 → 重启 → 验证版本"全链路。
4. **阶段 3**：加 token 认证、OTA 进度回传、失败自动回滚日志。
