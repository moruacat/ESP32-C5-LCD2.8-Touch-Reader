#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * GB2312 → Unicode 映射项
 *
 * 数据由 tools/gen_gbk_table.py 生成，按 gbk 字段升序排列，可二分查找。
 */
typedef struct {
    uint16_t gbk;   /*!< GBK 双字节编码（高字节在前拼成 16 位） */
    uint16_t uni;   /*!< 对应 Unicode 码点 */
} gbk_pair_t;

extern const gbk_pair_t g_gbk_table[];
extern const int        g_gbk_table_size;

#ifdef __cplusplus
}
#endif
