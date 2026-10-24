#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 单词页（查词 + 词级直译）
 *
 * 词库来自 srv_dict（SD 卡 /dict/dict.dat + dict.idx）。标题栏右侧两个模式标签：
 *
 *   - 查词：点搜索框弹屏幕键盘 → 整词命中（含词形还原，如 running→run）直接进
 *     详情（词 + 徽章 + 音标 + 释义，词形提示可点着跳原形）；没命中才铺列表，
 *     最上面是「是不是要找 xxx」的拼写建议，下面是以输入开头的词
 *   - 译：输入英文句子 → 逐词给中文首义（词形还原后查词，没收录的原样留空），
 *     点某一行进该词的完整词条，返回回到直译结果
 */
void ui_words_create(lv_obj_t *root);

#ifdef __cplusplus
}
#endif
