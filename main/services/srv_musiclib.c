#include "srv_musiclib.h"

#include "srv_sd.h"

#include "esp_log.h"
#include "esp_heap_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "srv_musiclib";

#define OTHERS_FILE     "others.dat"
#define PL_PREFIX       "playlists."
#define PL_SUFFIX       ".dat"

/* 播放列表文件名走常量表：序号拼进字符串会被 -Werror=format-truncation 拦 */
static const char *const PL_FILES[SRV_MUSIC_LIST_MAX] = {
    "playlists.1.dat",  "playlists.2.dat",  "playlists.3.dat",  "playlists.4.dat",
    "playlists.5.dat",  "playlists.6.dat",  "playlists.7.dat",  "playlists.8.dat",
    "playlists.9.dat",  "playlists.10.dat", "playlists.11.dat", "playlists.12.dat",
    "playlists.13.dat", "playlists.14.dat", "playlists.15.dat", "playlists.16.dat",
};

/* 扩展名 -> 格式；顺序无关，仅作查表 */
static const struct {
    const char      *ext;
    srv_music_fmt_t  fmt;
} EXT_TAB[] = {
    {"mp3",  SRV_MUSIC_FMT_MP3},
    {"wav",  SRV_MUSIC_FMT_WAV},
    {"aac",  SRV_MUSIC_FMT_AAC},
    {"m4a",  SRV_MUSIC_FMT_M4A},
    {"flac", SRV_MUSIC_FMT_FLAC},
    {"ogg",  SRV_MUSIC_FMT_OGG},
    {"opus", SRV_MUSIC_FMT_OGG},
    {"amr",  SRV_MUSIC_FMT_AMR},
    {"ts",   SRV_MUSIC_FMT_TS},
};

static char s_root[64];
static char s_others[96];

static srv_music_t *s_music = NULL;         /* 媒体库 */
static int          s_count = 0;

static srv_music_t *s_pl = NULL;            /* 已打开的播放列表 */
static int          s_pl_count = 0;
static int          s_pl_open  = 0;

static uint16_t     s_pl_mask = 0;          /* 存在的播放列表位图 */
static int          s_pl_ids[SRV_MUSIC_LIST_MAX];
static int          s_pl_cnt = 0;

/* 重新扫 /music 下有哪些 playlists.N.dat（定义在后面） */
static void pl_rescan(void);

/* 一个播放列表都没有时补建一个（定义在后面） */
static void ensure_playlist(void);

/* ===================== 路径 ===================== */

const char *srv_musiclib_root(void)
{
    if (s_root[0] == '\0') {
        snprintf(s_root, sizeof(s_root), "%s/music", srv_sd_mount_point());
    }
    return s_root;
}

const char *srv_musiclib_others_path(void)
{
    if (s_others[0] == '\0') {
        (void)srv_fs_join(srv_musiclib_root(), OTHERS_FILE, s_others, sizeof(s_others));
    }
    return s_others;
}

esp_err_t srv_musiclib_playlist_path(int id, char *out, size_t out_len)
{
    if (out == NULL || id < 1 || id > SRV_MUSIC_LIST_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    return srv_fs_join(srv_musiclib_root(), PL_FILES[id - 1], out, out_len);
}

/* ===================== 小工具 ===================== */

static esp_err_t alloc_buf(srv_music_t **pp)
{
    if (*pp != NULL) {
        return ESP_OK;
    }
    const size_t bytes = sizeof(srv_music_t) * SRV_MUSIC_MAX;

    /* 媒体库约 190KB，放 PSRAM；失败再退回内部 RAM */
    *pp = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (*pp == NULL) {
        ESP_LOGW(TAG, "PSRAM 分配失败，改用内部 RAM（%u 字节）", (unsigned)bytes);
        *pp = malloc(bytes);
    }
    if (*pp == NULL) {
        ESP_LOGE(TAG, "内存分配失败（%u 字节）", (unsigned)bytes);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static srv_music_fmt_t fmt_from_ext(const char *ext)
{
    for (size_t i = 0; i < sizeof(EXT_TAB) / sizeof(EXT_TAB[0]); i++) {
        if (strcasecmp(ext, EXT_TAB[i].ext) == 0) {
            return EXT_TAB[i].fmt;
        }
    }
    return SRV_MUSIC_FMT_UNKNOWN;
}

/* 显示名 = 文件名去掉最后的扩展名 */
static void make_display_name(const char *file_name, char *out, size_t out_len)
{
    snprintf(out, out_len, "%s", file_name);
    char *dot = strrchr(out, '.');
    if (dot != NULL && dot != out) {
        *dot = '\0';
    }
}

/* 把一条绝对路径填成曲目；不支持的扩展名返回 false */
static bool fill_item(const char *path, uint32_t size, bool external, srv_music_t *out)
{
    char ext[8];
    srv_fs_ext(path, ext, sizeof(ext));

    srv_music_fmt_t fmt = fmt_from_ext(ext);
    if (fmt == SRV_MUSIC_FMT_UNKNOWN) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->fmt      = fmt;
    out->external = external;
    out->size     = size;
    snprintf(out->path, sizeof(out->path), "%s", path);
    make_display_name(srv_fs_basename(path), out->name, sizeof(out->name));
    return true;
}

static srv_music_t *music_find_slot(const char *path)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_music[i].path, path) == 0) {
            return &s_music[i];
        }
    }
    return NULL;
}

static bool music_add(const char *path, uint32_t size, bool external)
{
    if (music_find_slot(path) != NULL) {
        return false;
    }
    if (s_count >= SRV_MUSIC_MAX) {
        ESP_LOGW(TAG, "媒体库已满（上限 %d），忽略 %s", SRV_MUSIC_MAX, path);
        return false;
    }
    if (!fill_item(path, size, external, &s_music[s_count])) {
        return false;
    }
    s_count++;
    return true;
}

static int cmp_name(const void *a, const void *b)
{
    return strcasecmp(((const srv_music_t *)a)->name, ((const srv_music_t *)b)->name);
}

/* ===================== 扫描 ===================== */

static bool scan_dir_cb(const srv_fs_entry_t *e, void *arg)
{
    (void)arg;
    if (e->is_dir) {
        return true;                    /* 只扫 /music 一层 */
    }

    char path[SRV_FS_PATH_MAX];
    if (srv_fs_join(srv_musiclib_root(), e->name, path, sizeof(path)) != ESP_OK) {
        return true;
    }
    music_add(path, e->size, false);
    return true;
}

static void others_load(void)
{
    FILE *fp = fopen(srv_musiclib_others_path(), "r");
    if (fp == NULL) {
        return;                         /* 首次使用没有该文件是正常的 */
    }

    char line[SRV_FS_PATH_MAX + 8];
    int  added = 0;

    while (fgets(line, sizeof(line), fp) != NULL) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0 || line[0] == '#') {
            continue;
        }

        srv_fs_entry_t e;
        if (srv_fs_stat(line, &e) != ESP_OK || e.is_dir) {
            ESP_LOGW(TAG, "外部曲目不存在，跳过: %s", line);
            continue;
        }
        if (music_add(line, e.size, true)) {
            added++;
        }
    }
    fclose(fp);

    if (added > 0) {
        ESP_LOGI(TAG, "载入外部曲目 %d 首", added);
    }
}

static esp_err_t others_save(void)
{
    FILE *fp = fopen(srv_musiclib_others_path(), "w");
    if (fp == NULL) {
        ESP_LOGE(TAG, "无法写入 %s", srv_musiclib_others_path());
        return ESP_FAIL;
    }

    fprintf(fp, "# 音乐库外部曲目清单，每行一个绝对路径（不移动源文件）\n");
    for (int i = 0; i < s_count; i++) {
        if (s_music[i].external) {
            fprintf(fp, "%s\n", s_music[i].path);
        }
    }
    fclose(fp);
    return ESP_OK;
}

/* ===================== 对外：媒体库 ===================== */

esp_err_t srv_musiclib_scan(void)
{
    if (alloc_buf(&s_music) != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    s_count = 0;

    if (!srv_sd_is_mounted()) {
        ESP_LOGW(TAG, "SD 未挂载，媒体库为空");
        /* 卡都没了，打开着的列表也一起清掉，免得界面还显示上一张卡的内容 */
        s_pl_open  = 0;
        s_pl_count = 0;
        pl_rescan();
        return ESP_ERR_INVALID_STATE;
    }

    srv_fs_mkdir(srv_musiclib_root());

    srv_fs_list(srv_musiclib_root(), scan_dir_cb, NULL);
    int from_dir = s_count;
    others_load();

    if (s_count > 1) {
        qsort(s_music, s_count, sizeof(srv_music_t), cmp_name);
    }

    pl_rescan();
    /* 每次扫完都确认一下有列表可用。
     * 开机时 SD 还没插的话，init 里那次补建是失败的，得靠这里
     * （后面「重载 SD 卡」再扫一次时）把列表补上，否则音乐页永远是空队列。 */
    ensure_playlist();

    ESP_LOGI(TAG, "媒体库扫描完成：%s 下 %d 首，外部 %d 首，共 %d 首",
             srv_musiclib_root(), from_dir, s_count - from_dir, s_count);
    return ESP_OK;
}

esp_err_t srv_musiclib_init(void)
{
    (void)srv_musiclib_root();
    (void)srv_musiclib_others_path();

    return srv_musiclib_scan();
}

int srv_musiclib_count(void)
{
    return s_count;
}

esp_err_t srv_musiclib_get(int idx, const srv_music_t **out)
{
    if (out == NULL || idx < 0 || idx >= s_count) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = &s_music[idx];
    return ESP_OK;
}

esp_err_t srv_musiclib_find(const char *path, const srv_music_t **out)
{
    if (path == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    srv_music_t *m = music_find_slot(path);
    if (m == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    *out = m;
    return ESP_OK;
}

esp_err_t srv_musiclib_add_external(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!srv_sd_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (alloc_buf(&s_music) != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    if (music_find_slot(path) != NULL) {
        return ESP_OK;                  /* 已在库里，幂等返回 */
    }

    srv_fs_entry_t e;
    if (srv_fs_stat(path, &e) != ESP_OK || e.is_dir) {
        ESP_LOGE(TAG, "路径不存在: %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    if (!music_add(path, e.size, true)) {
        ESP_LOGE(TAG, "不是受支持的音频文件: %s", path);
        return ESP_ERR_INVALID_ARG;
    }

    if (s_count > 1) {
        qsort(s_music, s_count, sizeof(srv_music_t), cmp_name);
    }
    return others_save();
}

esp_err_t srv_musiclib_remove(const char *path)
{
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    srv_music_t *m = music_find_slot(path);
    if (m == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    int idx = (int)(m - s_music);
    for (int i = idx; i < s_count - 1; i++) {
        s_music[i] = s_music[i + 1];
    }
    s_count--;

    ESP_LOGI(TAG, "已移出媒体库: %s", path);
    return others_save();               /* 更新 others.dat（非外部曲目不受影响） */
}

/* ===================== 播放列表 ===================== */

static bool pl_scan_cb(const srv_fs_entry_t *e, void *arg)
{
    uint16_t *mask = (uint16_t *)arg;

    if (e->is_dir) {
        return true;
    }
    if (strncmp(e->name, PL_PREFIX, strlen(PL_PREFIX)) != 0) {
        return true;
    }

    const char *p = e->name + strlen(PL_PREFIX);
    int v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        if (v > SRV_MUSIC_LIST_MAX) {
            return true;
        }
        p++;
    }
    if (v < 1 || v > SRV_MUSIC_LIST_MAX || strcasecmp(p, PL_SUFFIX) != 0) {
        return true;
    }

    *mask |= (uint16_t)(1u << (v - 1));
    return true;
}

static void pl_rescan(void)
{
    s_pl_mask = 0;
    srv_fs_list(srv_musiclib_root(), pl_scan_cb, &s_pl_mask);

    s_pl_cnt = 0;
    for (int i = 0; i < SRV_MUSIC_LIST_MAX; i++) {
        if (s_pl_mask & (1u << i)) {
            s_pl_ids[s_pl_cnt++] = i + 1;
        }
    }
}

int srv_musiclib_playlist_count(void)
{
    return s_pl_cnt;                    /* 由 scan / 增删时刷新，避免 UI 频繁读 SD */
}

int srv_musiclib_playlist_id(int slot)
{
    if (slot < 0 || slot >= s_pl_cnt) {
        return 0;
    }
    return s_pl_ids[slot];
}

esp_err_t srv_musiclib_playlist_create(int *out_id)
{
    pl_rescan();

    for (int i = 0; i < SRV_MUSIC_LIST_MAX; i++) {
        if (s_pl_mask & (1u << i)) {
            continue;
        }

        char path[SRV_FS_PATH_MAX];
        esp_err_t ret = srv_musiclib_playlist_path(i + 1, path, sizeof(path));
        if (ret != ESP_OK) {
            return ret;
        }

        FILE *fp = fopen(path, "w");
        if (fp == NULL) {
            ESP_LOGE(TAG, "无法创建 %s", path);
            return ESP_FAIL;
        }
        fprintf(fp, "# clock_reader 播放列表 %d\n", i + 1);
        fclose(fp);

        pl_rescan();
        if (out_id != NULL) {
            *out_id = i + 1;
        }
        ESP_LOGI(TAG, "新建播放列表 %d", i + 1);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "播放列表已达上限 %d", SRV_MUSIC_LIST_MAX);
    return ESP_ERR_NO_MEM;
}

/* 一个列表都没有时补建一个；SD 没挂载就写不出文件，直接算了 */
static void ensure_playlist(void)
{
    if (s_pl_cnt > 0 || !srv_sd_is_mounted()) {
        return;
    }
    int id = 0;
    (void)srv_musiclib_playlist_create(&id);
}

esp_err_t srv_musiclib_playlist_delete(int id)
{
    if (id < 1 || id > SRV_MUSIC_LIST_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    char path[SRV_FS_PATH_MAX];
    esp_err_t ret = srv_musiclib_playlist_path(id, path, sizeof(path));
    if (ret != ESP_OK) {
        return ret;
    }

    ret = srv_fs_remove(path);

    /* 删的正好是打开着的那个：内存里的副本也清掉 */
    if (s_pl_open == id) {
        s_pl_open  = 0;
        s_pl_count = 0;
    }
    pl_rescan();

    ESP_LOGI(TAG, "删除播放列表 %d", id);
    return ret;
}

static esp_err_t playlist_save(void)
{
    if (s_pl_open == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    char path[SRV_FS_PATH_MAX];
    esp_err_t ret = srv_musiclib_playlist_path(s_pl_open, path, sizeof(path));
    if (ret != ESP_OK) {
        return ret;
    }

    FILE *fp = fopen(path, "w");
    if (fp == NULL) {
        ESP_LOGE(TAG, "无法写入 %s", path);
        return ESP_FAIL;
    }

    fprintf(fp, "# clock_reader 播放列表 %d\n", s_pl_open);
    for (int i = 0; i < s_pl_count; i++) {
        fprintf(fp, "%s\n", s_pl[i].path);
    }
    fclose(fp);
    return ESP_OK;
}

esp_err_t srv_musiclib_playlist_open(int id)
{
    if (id < 1 || id > SRV_MUSIC_LIST_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (alloc_buf(&s_pl) != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    s_pl_count = 0;
    s_pl_open  = 0;

    char path[SRV_FS_PATH_MAX];
    esp_err_t ret = srv_musiclib_playlist_path(id, path, sizeof(path));
    if (ret != ESP_OK) {
        return ret;
    }

    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        ESP_LOGW(TAG, "播放列表 %d 不存在，按空列表处理", id);
        s_pl_open = id;
        return ESP_OK;
    }

    char line[SRV_FS_PATH_MAX + 8];
    while (fgets(line, sizeof(line), fp) != NULL) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0 || line[0] == '#') {
            continue;
        }
        if (s_pl_count >= SRV_MUSIC_MAX) {
            ESP_LOGW(TAG, "播放列表已满（上限 %d），忽略后续条目", SRV_MUSIC_MAX);
            break;
        }

        /* 优先复用媒体库里的信息；找不到就现场补一条（源文件不在则跳过） */
        const srv_music_t *hit = NULL;
        if (srv_musiclib_find(line, &hit) == ESP_OK) {
            s_pl[s_pl_count++] = *hit;
            continue;
        }

        srv_fs_entry_t e;
        if (srv_fs_stat(line, &e) != ESP_OK || e.is_dir) {
            ESP_LOGW(TAG, "播放列表条目已失效，跳过: %s", line);
            continue;
        }
        if (fill_item(line, e.size, false, &s_pl[s_pl_count])) {
            s_pl_count++;
        }
    }
    fclose(fp);

    s_pl_open = id;
    ESP_LOGI(TAG, "打开播放列表 %d：共 %d 首", id, s_pl_count);
    return ESP_OK;
}

int srv_musiclib_playlist_opened(void)
{
    return s_pl_open;
}

int srv_musiclib_playlist_item_count(void)
{
    return s_pl_count;
}

esp_err_t srv_musiclib_playlist_item(int idx, const srv_music_t **out)
{
    if (out == NULL || idx < 0 || idx >= s_pl_count) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = &s_pl[idx];
    return ESP_OK;
}

/* 把一条路径变成播放列表条目（优先用媒体库里的） */
static esp_err_t make_pl_item(const char *path, srv_music_t *dst)
{
    const srv_music_t *hit = NULL;
    if (srv_musiclib_find(path, &hit) == ESP_OK) {
        *dst = *hit;
        return ESP_OK;
    }

    srv_fs_entry_t e;
    if (srv_fs_stat(path, &e) != ESP_OK || e.is_dir) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!fill_item(path, e.size, false, dst)) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t srv_musiclib_playlist_append(const char *path)
{
    if (path == NULL || s_pl_open == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_pl_count >= SRV_MUSIC_MAX) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = make_pl_item(path, &s_pl[s_pl_count]);
    if (ret != ESP_OK) {
        return ret;
    }
    s_pl_count++;
    return playlist_save();
}

esp_err_t srv_musiclib_playlist_remove_item(int idx)
{
    if (s_pl_open == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (idx < 0 || idx >= s_pl_count) {
        return ESP_ERR_INVALID_ARG;
    }

    for (int i = idx; i < s_pl_count - 1; i++) {
        s_pl[i] = s_pl[i + 1];
    }
    s_pl_count--;
    return playlist_save();
}

esp_err_t srv_musiclib_playlist_move(int from, int to)
{
    if (s_pl_open == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (from < 0 || from >= s_pl_count || to < 0 || to >= s_pl_count || from == to) {
        return ESP_ERR_INVALID_ARG;
    }

    srv_music_t tmp = s_pl[from];
    if (from < to) {
        for (int i = from; i < to; i++) {
            s_pl[i] = s_pl[i + 1];
        }
    } else {
        for (int i = from; i > to; i--) {
            s_pl[i] = s_pl[i - 1];
        }
    }
    s_pl[to] = tmp;
    return playlist_save();
}

esp_err_t srv_musiclib_playlist_clear(void)
{
    if (s_pl_open == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    s_pl_count = 0;
    return playlist_save();
}
