#include "srv_fs.h"

#include "esp_log.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "srv_fs";

/* ===================== 查询 ===================== */

esp_err_t srv_fs_list(const char *path, srv_fs_list_cb_t cb, void *arg)
{
    if (path == NULL || cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    DIR *dir = opendir(path);
    if (dir == NULL) {
        ESP_LOGW(TAG, "打不开目录: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    char          full[SRV_FS_PATH_MAX];
    srv_fs_entry_t entry;
    struct dirent *de;

    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }

        if (srv_fs_join(path, de->d_name, full, sizeof(full)) != ESP_OK) {
            ESP_LOGW(TAG, "路径过长，跳过: %s", de->d_name);
            continue;
        }

        if (srv_fs_stat(full, &entry) != ESP_OK) {
            ESP_LOGW(TAG, "取信息失败，跳过: %s", full);
            continue;
        }

        if (!cb(&entry, arg)) {
            break;                      /* 调用方要求提前结束 */
        }
    }

    closedir(dir);
    return ESP_OK;
}

esp_err_t srv_fs_stat(const char *path, srv_fs_entry_t *out)
{
    if (path == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    memset(out, 0, sizeof(*out));
    out->is_dir = S_ISDIR(st.st_mode);
    out->size   = out->is_dir ? 0 : (uint32_t)st.st_size;

    int n = snprintf(out->name, sizeof(out->name), "%s", srv_fs_basename(path));
    return (n > 0 && (size_t)n < sizeof(out->name)) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

bool srv_fs_exists(const char *path)
{
    struct stat st;
    return (path != NULL) && (stat(path, &st) == 0);
}

bool srv_fs_is_dir(const char *path)
{
    struct stat st;
    return (path != NULL) && (stat(path, &st) == 0) && S_ISDIR(st.st_mode);
}

/* ===================== 修改 ===================== */

esp_err_t srv_fs_mkdir(const char *path)
{
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (mkdir(path, 0777) == 0) {
        return ESP_OK;
    }
    /* 已存在不算失败 */
    return srv_fs_is_dir(path) ? ESP_OK : ESP_FAIL;
}

esp_err_t srv_fs_remove(const char *path)
{
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (remove(path) == 0) {
        return ESP_OK;
    }
    return rmdir(path);
}

esp_err_t srv_fs_rename(const char *from, const char *to)
{
    if (from == NULL || to == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return (rename(from, to) == 0) ? ESP_OK : ESP_FAIL;
}

/* 递归删除用：把当前目录路径带给回调 */
typedef struct {
    char dir[SRV_FS_PATH_MAX];
} rm_ctx_t;

static bool rm_child_cb(const srv_fs_entry_t *entry, void *arg)
{
    rm_ctx_t *ctx = (rm_ctx_t *)arg;

    char child[SRV_FS_PATH_MAX];
    if (srv_fs_join(ctx->dir, entry->name, child, sizeof(child)) != ESP_OK) {
        return false;
    }
    return srv_fs_remove_tree(child) == ESP_OK;
}

esp_err_t srv_fs_remove_tree(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (!srv_fs_is_dir(path)) {
        return srv_fs_remove(path);
    }

    /* 先把里面的东西删干净，再删目录本身（EBF 书是文件夹，内含章节文件）。
     * 目录层数很浅（EBF 只有一层），用栈上的上下文递归就够。 */
    rm_ctx_t ctx;
    int n = snprintf(ctx.dir, sizeof(ctx.dir), "%s", path);
    if (n <= 0 || (size_t)n >= sizeof(ctx.dir)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (srv_fs_list(ctx.dir, rm_child_cb, &ctx) != ESP_OK) {
        return ESP_FAIL;
    }
    return srv_fs_remove(path);
}

/* ===================== 路径工具 ===================== */

esp_err_t srv_fs_join(const char *dir, const char *name, char *out, size_t out_len)
{
    if (name == NULL || out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    dir = (dir == NULL) ? "" : dir;

    /* 去掉 dir 结尾重复的 '/'（保留根目录本身的单个 '/'） */
    size_t dir_len = strlen(dir);
    while (dir_len > 1 && dir[dir_len - 1] == '/') {
        dir_len--;
    }

    /* 去掉 name 开头的 '/' */
    while (*name == '/') {
        name++;
    }

    int n;
    if (dir_len == 0) {
        n = snprintf(out, out_len, "%s", name);
    } else if (dir_len == 1 && dir[0] == '/') {
        n = snprintf(out, out_len, "/%s", name);
    } else {
        n = snprintf(out, out_len, "%.*s/%s", (int)dir_len, dir, name);
    }

    return (n > 0 && (size_t)n < out_len) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

void srv_fs_parent(const char *path, char *out, size_t out_len)
{
    if (path == NULL || out == NULL || out_len == 0) {
        return;
    }

    size_t len = strlen(path);
    /* 去掉结尾的 '/' */
    while (len > 1 && path[len - 1] == '/') {
        len--;
    }

    size_t cut = len;
    while (cut > 0 && path[cut - 1] != '/') {
        cut--;
    }
    /* cut 指向最后一个 '/' 之后；再去掉该 '/' */
    if (cut > 1) {
        cut--;
    }

    if (cut == 0) {
        snprintf(out, out_len, "/");
        return;
    }
    snprintf(out, out_len, "%.*s", (int)cut, path);
}

const char *srv_fs_basename(const char *path)
{
    if (path == NULL) {
        return "";
    }

    const char *slash = strrchr(path, '/');
    return (slash == NULL) ? path : (slash + 1);
}

void srv_fs_ext(const char *path, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    out[0] = '\0';

    if (path == NULL) {
        return;
    }

    const char *base = srv_fs_basename(path);
    const char *dot  = strrchr(base, '.');
    if (dot == NULL || dot == base || dot[1] == '\0') {
        return;                         /* 无扩展名，或形如 ".gitignore" */
    }

    size_t i = 0;
    for (const char *p = dot + 1; *p != '\0' && i + 1 < out_len; p++, i++) {
        char c = *p;
        out[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    out[i] = '\0';
}
