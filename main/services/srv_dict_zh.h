#pragma once
#include "esp_err.h"
#include "srv_dict.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 中英双向词典（CC-CEDICT）。
 *
 * 数据由 tools/build_zh_dict.py 生成，放在 SD 卡：
 *   /sdcard/dict/zhdict.dat      中文词 \t 拼音 \t 英文释义 \t flags \t lemma
 *   /sdcard/dict/zhdict.idx      主索引（按中文词排序，每 64 行一个锚点）
 *   /sdcard/dict/zhdict_py.idx   拼音索引（无声调，每 64 行一个锚点 + 行号表）
 *
 * 只常驻两个锚点表（约 48KB），正文按需读 SD；查一个词最多读 64 行。
 * 与 srv_dict 共用 srv_dict_entry_t，UI 层不用区分两种词典。
 */

/** 打开索引并常开正文文件；SD 不在、文件缺失时返回错误（不影响开机） */
esp_err_t srv_dict_zh_init(void);

/** 索引是否就绪 */
bool srv_dict_zh_ready(void);

/** 中文词精确查找 */
esp_err_t srv_dict_zh_lookup(const char *word, srv_dict_entry_t *out);

/** 中文前缀查找（输入「苹果」列出「苹果派」这类），按词序输出 */
int srv_dict_zh_prefix(const char *prefix, srv_dict_list_cb_t cb, void *arg, int max);

/** 无声调拼音查找（pingguo / chuan tong），按拼音序输出 */
int srv_dict_zh_pinyin(const char *pinyin, srv_dict_list_cb_t cb, void *arg, int max);

#ifdef __cplusplus
}
#endif
