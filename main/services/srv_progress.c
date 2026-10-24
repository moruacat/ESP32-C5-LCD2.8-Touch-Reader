#include "srv_progress.h"

#include "srv_sd.h"
#include "srv_fs.h"

#include "esp_log.h"
#include "esp_timer.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "srv_progress";

/* 非强制保存的最小间隔：避免阅读翻页/播放中高频写 SD */
#define SAVE_MIN_INTERVAL_US   (5 * 1000 * 1000)

#define LINE_MAX               192

/*! EBF 每章各记一份阅读位置的行数上限（够用就好，超出的章不再单独记） */
#define PROGRESS_MAX_CHAPTERS  256

static char s_music_path[80];

/* 读写行时间戳：按路径分别记最后一次写盘时间 */
static int64_t s_reader_last_us = 0;
static int64_t s_music_last_us  = 0;

/*! 每章位置的临时表（-1 = 该章没有记录）。
 *  放 static 而不是栈上：调用方可能跑在只有 4KB 栈的后台打开任务里。 */
static int32_t s_ch_offs[PROGRESS_MAX_CHAPTERS];

/* ===================== 工具 ===================== */

static void music_path_init(void)
{
    if (s_music_path[0] == '\0') {
        snprintf(s_music_path, sizeof(s_music_path), "%s/music_progress.dat", srv_sd_mount_point());
    }
}

const char *srv_progress_music_path(void)
{
    music_path_init();
    return s_music_path;
}

/* EBF 是目录 -> progress.dat；TXT 是文件 -> <path>.progress.dat */
static void reader_path(const char *book_path, char *out, size_t out_len)
{
    if (srv_fs_is_dir(book_path)) {
        snprintf(out, out_len, "%s/progress.dat", book_path);
    } else {
        snprintf(out, out_len, "%s.progress.dat", book_path);
    }
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        s++;
    }
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n')) {
        *--end = '\0';
    }
    return s;
}

/* 通用 key=value 行解析：命中 key 时把整数写入 *dst */
static bool match_int(const char *line, const char *key, int32_t *dst)
{
    size_t klen = strlen(key);
    if (strncmp(line, key, klen) != 0 || line[klen] != '=') {
        return false;
    }
    *dst = (int32_t)strtol(line + klen + 1, NULL, 10);
    return true;
}

static void ch_offs_reset(void)
{
    for (int i = 0; i < PROGRESS_MAX_CHAPTERS; i++) {
        s_ch_offs[i] = -1;
    }
}

/**
 * @brief 解析一行「o<章号>=<章内字符偏移>」
 *
 * EBF 每章各记一份位置：跳到别的章读过之后回到原来那一章，仍停在原处。
 * 章号从 1 开始（chapter.N.ebfl 的编号），表下标是章号 - 1。
 */
static void parse_chapter_off(const char *line)
{
    if (line[0] != 'o' || !isdigit((unsigned char)line[1])) {
        return;
    }
    const char *eq = strchr(line, '=');
    if (eq == NULL) {
        return;
    }

    int ch  = (int)strtol(line + 1, NULL, 10);
    int off = (int)strtol(eq + 1, NULL, 10);
    if (ch >= 1 && ch <= PROGRESS_MAX_CHAPTERS && off >= 0) {
        s_ch_offs[ch - 1] = off;
    }
}

/**
 * @brief 读一份进度文件
 * @param out   完整进度结构
 * @param offs  是否顺带收集「各章位置」（EBF 才需要）
 */
static esp_err_t progress_read(const char *path, srv_progress_reader_t *out, bool offs)
{
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    char line[LINE_MAX];
    int  hit = 0;
    while (fgets(line, sizeof(line), fp) != NULL) {
        char *p = trim(line);
        if (*p == '\0' || *p == '#') {
            continue;
        }
        if (match_int(p, "chapter",    &out->chapter)    ||
            match_int(p, "page",       &out->page)       ||
            match_int(p, "line_off",   &out->line_off)   ||
            match_int(p, "byte_off",   &out->byte_off)   ||
            match_int(p, "pct",        &out->pct)        ||
            match_int(p, "font_size",  &out->font_size)  ||
            match_int(p, "line_space", &out->line_space)) {
            hit++;
            continue;
        }
        if (offs) {
            parse_chapter_off(p);
        }
    }
    fclose(fp);

    return (hit > 0) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

/* ===================== 阅读进度 ===================== */

esp_err_t srv_progress_reader_load(const char *book_path, srv_progress_reader_t *out)
{
    if (book_path == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    char path[SRV_FS_PATH_MAX];
    reader_path(book_path, path, sizeof(path));

    /* 只有 EBF 才按章记位置；TXT 一份就够，保持原样 */
    const bool per_chapter = srv_fs_is_dir(book_path);
    ch_offs_reset();

    esp_err_t ret = progress_read(path, out, per_chapter);
    if (ret == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "无阅读进度: %s", path);
        return ret;
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "%s 内容无效，视为无进度", path);
        memset(out, 0, sizeof(*out));
        return ret;
    }

    /* 用「当前这一章」自己那份位置：跳章读过之后回来还停在原处 */
    if (per_chapter && out->chapter >= 0 && out->chapter < PROGRESS_MAX_CHAPTERS &&
        s_ch_offs[out->chapter] >= 0) {
        out->byte_off = s_ch_offs[out->chapter];
    }

    /* 关键字段做区间保护，防止被改坏后越界 */
    if (out->pct < 0)     out->pct = 0;
    if (out->pct > 100)   out->pct = 100;
    if (out->chapter < 0) out->chapter = 0;
    if (out->page < 0)    out->page = 0;
    if (out->byte_off < 0) out->byte_off = 0;

    return ESP_OK;
}

esp_err_t srv_progress_reader_save(const char *book_path, const srv_progress_reader_t *in, bool force)
{
    if (book_path == NULL || in == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!srv_sd_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    int64_t now = esp_timer_get_time();
    if (!force && (now - s_reader_last_us) < SAVE_MIN_INTERVAL_US) {
        return ESP_OK;              /* 节流：本轮跳过 */
    }

    char path[SRV_FS_PATH_MAX];
    reader_path(book_path, path, sizeof(path));

    /* EBF：先把文件里已有的各章位置收回来，替换掉当前章那一行再整份写回 ——
     * 不这么做的话，跳到别的章读一会儿就会把之前那些章的位置冲掉。 */
    const bool per_chapter = srv_fs_is_dir(book_path);
    ch_offs_reset();
    if (per_chapter) {
        srv_progress_reader_t old = {0};
        (void)progress_read(path, &old, true);
        if (in->chapter >= 0 && in->chapter < PROGRESS_MAX_CHAPTERS) {
            s_ch_offs[in->chapter] = in->byte_off;
        }
    }

    FILE *fp = fopen(path, "w");
    if (fp == NULL) {
        ESP_LOGE(TAG, "无法写入 %s", path);
        return ESP_FAIL;
    }

    fprintf(fp, "# clock_reader 阅读进度\n");
    fprintf(fp, "chapter=%ld\n",    (long)in->chapter);
    fprintf(fp, "page=%ld\n",       (long)in->page);
    fprintf(fp, "line_off=%ld\n",   (long)in->line_off);
    fprintf(fp, "byte_off=%ld\n",   (long)in->byte_off);
    fprintf(fp, "pct=%ld\n",        (long)in->pct);
    fprintf(fp, "font_size=%ld\n",  (long)in->font_size);
    fprintf(fp, "line_space=%ld\n", (long)in->line_space);

    if (per_chapter) {
        for (int i = 0; i < PROGRESS_MAX_CHAPTERS; i++) {
            if (s_ch_offs[i] >= 0) {
                fprintf(fp, "o%d=%ld\n", i + 1, (long)s_ch_offs[i]);
            }
        }
    }
    fclose(fp);

    s_reader_last_us = now;
    ESP_LOGD(TAG, "阅读进度已保存: %s", path);
    return ESP_OK;
}

esp_err_t srv_progress_reader_clear(const char *book_path)
{
    if (book_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[SRV_FS_PATH_MAX];
    reader_path(book_path, path, sizeof(path));
    return srv_fs_remove(path);
}

/* ===================== 音乐进度 ===================== */

esp_err_t srv_progress_music_load(srv_progress_music_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    music_path_init();

    FILE *fp = fopen(s_music_path, "r");
    if (fp == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    char line[LINE_MAX];
    int  hit = 0;
    while (fgets(line, sizeof(line), fp) != NULL) {
        char *p = trim(line);
        if (*p == '\0' || *p == '#') {
            continue;
        }
        if (match_int(p, "playlist",    &out->playlist) ||
            match_int(p, "playing",     &out->playing)  ||
            match_int(p, "index",       &out->index)    ||
            match_int(p, "mode",        &out->mode)) {
            hit++;
            continue;
        }
        if (strncmp(p, "file=", 5) == 0) {
            snprintf(out->file, sizeof(out->file), "%s", p + 5);
            hit++;
        }
    }
    fclose(fp);

    if (hit == 0) {
        memset(out, 0, sizeof(*out));
        return ESP_ERR_INVALID_STATE;
    }

    if (out->playlist < 0)    out->playlist = 0;
    if (out->index < 0)       out->index = 0;
    out->playing = out->playing ? 1 : 0;

    return ESP_OK;
}

esp_err_t srv_progress_music_save(const srv_progress_music_t *in, bool force)
{
    if (in == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!srv_sd_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    music_path_init();

    int64_t now = esp_timer_get_time();
    if (!force && (now - s_music_last_us) < SAVE_MIN_INTERVAL_US) {
        return ESP_OK;
    }

    FILE *fp = fopen(s_music_path, "w");
    if (fp == NULL) {
        ESP_LOGE(TAG, "无法写入 %s", s_music_path);
        return ESP_FAIL;
    }

    fprintf(fp, "# clock_reader 音乐播放进度\n");
    fprintf(fp, "playlist=%ld\n",    (long)in->playlist);
    fprintf(fp, "playing=%ld\n",     (long)in->playing);
    fprintf(fp, "index=%ld\n",       (long)in->index);
    fprintf(fp, "mode=%ld\n",        (long)in->mode);
    fprintf(fp, "file=%s\n",         in->file);
    fclose(fp);

    s_music_last_us = now;
    return ESP_OK;
}