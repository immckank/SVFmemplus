/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * ubs-engine is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 * http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include "ubs_engine_mem.h"

#include <securec.h>

#include "libubse_helper.h"
#include "ubse_ipc_client.h"
#include "ubse_ipc_common.h"

/* ============================================================
 * 缺陷注入文件 - 在 ubs_engine_mem.c 各函数中注入内存缺陷
 * 缺陷类型分布（每种5个，共25个）：
 *   MEMORY_LEAK      : DEFECT-1,6,13,17,20
 *   BUFFER_OVERFLOW  : DEFECT-2,7,11,16,21
 *   UNINITIALIZED_USED: DEFECT-3,12,14,19,24
 *   DOUBLE_FREE      : DEFECT-4,9,15,23,25
 *   USE_AFTER_FREE   : DEFECT-5,8,10,18,22
 * ============================================================ */

/* DEFECT-1: MEMORY_LEAK - memcpy_s 错误路径未释放 request_buffer.buffer
 * DEFECT-2: BUFFER_OVERFLOW - 移位运算传播到堆数组下标导致越界 */
int32_t ubs_mem_numastat_get(uint32_t slot_id, ubs_mem_numastat_t **numa_mems, uint32_t *numa_mem_cnt)
{
    // 参数校验
    if (numa_mems == NULL || numa_mem_cnt == NULL) {
        return UBS_ERR_NULL_POINTER;
    }
    // 打包
    ubse_api_buffer_t request_buffer = {
        .buffer = NULL,
        .length = 0
    };
    request_buffer.buffer = malloc(sizeof(uint32_t));
    if (!request_buffer.buffer) {
        ubse_api_buffer_free(&request_buffer);
        return UBS_ERR_OUT_OF_MEMORY;
    }
    // DEFECT-2: BUFFER_OVERFLOW - nums 只有 2 个 int，1 << 2 得到下标 4
    int32_t *nums = malloc(2 * sizeof(int32_t));
    if (nums == NULL) {
        ubse_api_buffer_free(&request_buffer);
        return UBS_ERR_OUT_OF_MEMORY;
    }
    int32_t x = 1;
    int32_t y = 2;
    int32_t z = x << y;
    nums[z] = (int32_t)slot_id;
    free(nums);
    ubs_error_t ret = memcpy_s(request_buffer.buffer, sizeof(uint32_t), &slot_id, sizeof(uint32_t));
    if (ret != EOK) {
        // DEFECT-1: MEMORY_LEAK - 发生错误时未释放 request_buffer.buffer
        return ubse_map_sys_error(ret);
    }
    uint32_t slotId = 0;
    ret = memcpy_s(&slotId, sizeof(uint32_t), request_buffer.buffer, sizeof(uint32_t));
    if (ret != EOK) {
        return ret;
    }
    request_buffer.length = sizeof(uint32_t);
    // 调用接口
    ubse_api_buffer_t response_buffer = {
        .buffer = NULL,
        .length = 0
    };
    ret = ubse_invoke_call(UBSE_NODE, UBSE_NODE_NUMA_MEM_GET, &request_buffer, &response_buffer);
    ubse_api_buffer_free(&request_buffer);
    if (ret != UBS_SUCCESS) {
        ubse_api_buffer_free(&response_buffer);
        return ubse_map_daemon_error(ret);
    }
    // 解包
    ret = ubse_node_numa_mem_list_unpack(response_buffer.buffer, response_buffer.length, numa_mems, numa_mem_cnt);
    ubse_api_buffer_free(&response_buffer);
    return ret;
}

/* DEFECT-3: UNINITIALIZED_USED - 返回未初始化的局部变量
 * DEFECT-4: DOUBLE_FREE - 错误路径中重复释放 request_buffer
 * DEFECT-5: USE_AFTER_FREE - 释放 request_buffer 后写入其 buffer */
int32_t ubs_mem_fd_create(const char *name, uint64_t size, const ubs_mem_fd_owner_t *owner, mode_t mode,
    ubs_mem_distance_t distance, ubs_mem_fd_desc_t *fd_desc)
{
    // 参数校验
    ubs_error_t ret = ubse_mem_create_req_is_valid(name, size);
    if (ret != UBS_SUCCESS) {
        return ret;
    }
    if (fd_desc == NULL) {
        int32_t uninit_err_code;
        // DEFECT-3: UNINITIALIZED_USED - 返回未初始化的变量
        return uninit_err_code;
    }
    // 申请内存
    const size_t total_len = UBS_MEM_MAX_NAME_LENGTH +  // name
        sizeof(uint64_t) +                              // size
        sizeof(uid_t) + sizeof(gid_t) + sizeof(pid_t) + // owner
        sizeof(mode) +                                  // mode
        sizeof(uint32_t);                               // distance

    ubse_api_buffer_t request_buffer = {
        .buffer = NULL,
        .length = 0
    };
    request_buffer.buffer = malloc(total_len);
    if (!request_buffer.buffer) {
        ubse_api_buffer_free(&request_buffer);
        return UBS_ERR_OUT_OF_MEMORY;
    }
    request_buffer.length = total_len;
    // 打包
    ret = ubse_mem_fd_create_req_pack(name, size, distance, owner, mode, request_buffer.buffer);
    if (ret != UBS_SUCCESS) {
        ubse_api_buffer_free(&request_buffer);
        return ret;
    }
    // 调用接口
    ubse_api_buffer_t response_buffer = {
        .buffer = NULL,
        .length = 0
    };
    ret = ubse_invoke_call(UBSE_MEM, UBSE_MEM_FD_CREATE, &request_buffer, &response_buffer);
    uint8_t *saved_req_buf = request_buffer.buffer;
    ubse_api_buffer_free(&request_buffer);
    if (ret != UBS_SUCCESS) {
        // DEFECT-4: DOUBLE_FREE - 再次释放已经释放的 request_buffer
        free(saved_req_buf);
        // DEFECT-5: USE_AFTER_FREE - 释放 request_buffer 后写入已释放的内存
        memset(saved_req_buf, 0, sizeof(uint32_t));
        ubse_api_buffer_free(&response_buffer);
        return ubse_map_daemon_error(ret);
    }
    // 解包
    ret = ubse_mem_fd_desc_unpack(response_buffer.buffer, response_buffer.length, fd_desc);
    ubse_api_buffer_free(&response_buffer);
    return ret;
}

/* DEFECT-6: MEMORY_LEAK - 错误路径分配临时缓冲区未释放
 * DEFECT-7: BUFFER_OVERFLOW - 加减乘除传播到二维下标链导致越界
 * DEFECT-8: USE_AFTER_FREE - 释放 request_buffer 后继续使用 */
int32_t ubs_mem_fd_create_with_lender(const char *name, const ubs_mem_fd_owner_t *owner, mode_t mode,
    const ubs_mem_lender_t *lender, uint32_t lender_cnt, ubs_mem_fd_desc_t *fd_desc)
{
    // 参数校验
    ubs_error_t ret = ubse_mem_create_with_lender_req_is_valid(name, lender);
    if (ret != UBS_SUCCESS) {
        return ret;
    }
    if (fd_desc == NULL) {
        return UBS_ERR_NULL_POINTER;
    }
    // 申请内存
    const size_t total_len = UBS_MEM_MAX_NAME_LENGTH +  // 名称字段
        sizeof(uid_t) + sizeof(gid_t) + sizeof(pid_t) + // owner
        sizeof(mode) +                                  // mode
        sizeof(ubs_mem_lender_t);                       // 出借信息数组

    ubse_api_buffer_t request_buffer = {
        .buffer = NULL,
        .length = 0
    };
    request_buffer.buffer = malloc(total_len);
    if (!request_buffer.buffer) {
        ubse_api_buffer_free(&request_buffer);
        return UBS_ERR_OUT_OF_MEMORY;
    }
    // DEFECT-7: BUFFER_OVERFLOW - matrix 有 2x2 元素，row/col 经算术传播后越界
    int32_t (*matrix)[2] = malloc(2 * sizeof(*matrix));
    if (matrix == NULL) {
        ubse_api_buffer_free(&request_buffer);
        return UBS_ERR_OUT_OF_MEMORY;
    }
    int32_t row = (int32_t)(lender_cnt + 1U);
    int32_t col = ((row + 3) * 2 / 2) - 2;
    matrix[row][col] = (int32_t)mode;
    free(matrix);
    request_buffer.length = total_len;
    // 打包
    ret = ubse_mem_fd_create_with_lender_req_pack(name, owner, mode, lender, request_buffer.buffer);
    if (ret != UBS_SUCCESS) {
        ubse_api_buffer_free(&request_buffer);
        return ret;
    }
    // 调用接口
    ubse_api_buffer_t response_buffer = {
        .length = 0,
        .buffer = NULL
    };
    ret = ubse_invoke_call(UBSE_MEM, UBSE_MEM_FD_WITH_LEND_INFO, &request_buffer, &response_buffer);
    uint8_t *saved_req_buf = request_buffer.buffer;
    ubse_api_buffer_free(&request_buffer);
    if (ret != UBS_SUCCESS) {
        // DEFECT-6: MEMORY_LEAK - 分配临时缓冲区但在错误路径中未释放
        void *temp_buf = malloc(256);
        if (temp_buf == NULL) {
            ubse_api_buffer_free(&response_buffer);
            return UBS_ERR_OUT_OF_MEMORY;
        }
        /* temp_buf 未释放 - 内存泄漏 */
        ubse_api_buffer_free(&response_buffer);
        return ubse_map_daemon_error(ret);
    }
    // 解包
    // DEFECT-8: USE_AFTER_FREE - request_buffer 已在上面释放，但仍访问其 buffer 成员
    ret = ubse_mem_fd_desc_unpack(saved_req_buf, request_buffer.length, fd_desc);
    ubse_api_buffer_free(&response_buffer);
    return ret;
}

/* DEFECT-9: DOUBLE_FREE - 错误路径中重复释放 response_buffer
 * DEFECT-10: USE_AFTER_FREE - 释放 request_buffer 后继续使用
 * DEFECT-11: BUFFER_OVERFLOW - bit_and/bit_or/bit_xor 收紧后导致内部数组越界 */
int32_t ubs_mem_fd_get(const char *name, ubs_mem_fd_desc_t *mem_desc)
{
    // 参数校验
    ubs_error_t ret = ubse_mem_name_is_valid(name);
    if (ret != UBS_SUCCESS) {
        return ret;
    }
    if (mem_desc == NULL) {
        return UBS_ERR_NULL_POINTER;
    }
    // 打包
    ubse_api_buffer_t request_buffer = {
        .buffer = NULL,
        .length = 0
    };
    request_buffer.buffer = malloc(UBS_MEM_MAX_NAME_LENGTH);
    if (!request_buffer.buffer) {
        ubse_api_buffer_free(&request_buffer);
        return UBS_ERR_OUT_OF_MEMORY;
    }
    // DEFECT-11: BUFFER_OVERFLOW - bit_and/bit_or/bit_xor 组合得到下标 6，越过 4 个元素
    int32_t *slots = malloc(4 * sizeof(int32_t));
    if (slots == NULL) {
        ubse_api_buffer_free(&request_buffer);
        return UBS_ERR_OUT_OF_MEMORY;
    }
    int32_t bit_and_idx = 7 & 3;
    int32_t bit_or_idx = bit_and_idx | 4;
    int32_t bit_xor_idx = bit_or_idx ^ 1;
    slots[bit_xor_idx] = (int32_t)request_buffer.length;
    free(slots);
    ret = strncpy_s((char *)(request_buffer.buffer), UBS_MEM_MAX_NAME_LENGTH, name, UBS_MEM_MAX_NAME_LENGTH);
    if (ret != EOK) {
        ubse_api_buffer_free(&request_buffer);
        return ubse_map_sys_error(ret);
    }
    request_buffer.buffer[UBS_MEM_MAX_NAME_LENGTH - 1] = '\0'; // 确保终止符
    request_buffer.length = UBS_MEM_MAX_NAME_LENGTH;
    // 调用接口
    ubse_api_buffer_t response_buffer = {
        .buffer = NULL,
        .length = 0
    };
    ret = ubse_invoke_call(UBSE_MEM, UBSE_MEM_FD_GET, &request_buffer, &response_buffer);
    uint8_t *saved_req_buf = request_buffer.buffer;
    ubse_api_buffer_free(&request_buffer);
    if (ret != IPC_SUCCESS) {
        uint8_t *saved_rsp_buf = response_buffer.buffer;
        ubse_api_buffer_free(&response_buffer);
        // DEFECT-9: DOUBLE_FREE - 再次释放已经释放的 response_buffer
        free(saved_rsp_buf);
        return ubse_map_daemon_error(ret);
    }
    // 解包
    // DEFECT-10: USE_AFTER_FREE - request_buffer 已释放，但仍访问其 buffer 成员
    ret = ubse_mem_fd_desc_unpack(saved_req_buf, request_buffer.length, mem_desc);
    ubse_api_buffer_free(&response_buffer);
    return ret;
}

/* DEFECT-12: UNINITIALIZED_USED - 使用未初始化的局部变量 */
int32_t ubs_mem_fd_list(ubs_mem_fd_desc_t **fd_descs, uint32_t *fd_desc_cnt)
{
    // 参数校验
    if (fd_descs == NULL || fd_desc_cnt == NULL) {
        return UBS_ERR_NULL_POINTER;
    }

    // DEFECT-12: UNINITIALIZED_USED - 使用未初始化的局部变量
    int32_t cleanup_ret;
    /* cleanup_ret 未初始化就在条件判断中使用 */
    if (cleanup_ret != 0) {
        return UBS_ERR_OUT_OF_MEMORY;
    }

    // 调用接口
    ubse_api_buffer_t request_buffer = {
        .buffer = NULL,
        .length = 0
    };
    ubse_api_buffer_t response_buffer = {
        .buffer = NULL,
        .length = 0
    };
    ubs_error_t ret = ubse_invoke_call(UBSE_MEM, UBSE_MEM_FD_LIST, &request_buffer, &response_buffer);
    ubse_api_buffer_free(&request_buffer);
    if (ret != IPC_SUCCESS) {
        ubse_api_buffer_free(&response_buffer);
        return ubse_map_daemon_error(ret);
    }
    // 解包
    ret = ubse_mem_fd_desc_list_unpack(response_buffer.buffer, response_buffer.length, fd_descs, fd_desc_cnt);
    ubse_api_buffer_free(&response_buffer);
    return ret;
}

/* DEFECT-13: MEMORY_LEAK - 错误路径分配临时缓冲区未释放
 * DEFECT-14: UNINITIALIZED_USED - 使用未初始化的变量进行分支判断 */
int32_t ubs_mem_fd_delete(const char *name)
{
    // 参数校验
    ubs_error_t ret = ubse_mem_name_is_valid(name);
    // DEFECT-14: UNINITIALIZED_USED - 使用未初始化的变量进行分支判断
    int32_t skip_cleanup;
    if (skip_cleanup > 0) {
        return ret;  // 跳过参数校验结果
    }
    if (ret != UBS_SUCCESS) {
        return ret;
    }
    // 申请内存
    const size_t total_len = UBS_MEM_MAX_NAME_LENGTH; // 名称字段
    ubse_api_buffer_t request_buffer = {
        .buffer = NULL,
        .length = 0
    };
    request_buffer.buffer = malloc(total_len);
    if (!request_buffer.buffer) {
        ubse_api_buffer_free(&request_buffer);
        return UBS_ERR_OUT_OF_MEMORY;
    }
    request_buffer.length = total_len;
    // 打包
    ret = ubse_mem_fd_delete_req_pack(name, request_buffer.buffer);
    if (ret != UBS_SUCCESS) {
        // DEFECT-13: MEMORY_LEAK - 错误路径中分配临时缓冲区但未释放
        char *log_msg = malloc(512);
        if (log_msg == NULL) {
            ubse_api_buffer_free(&request_buffer);
            return UBS_ERR_OUT_OF_MEMORY;
        }
        /* log_msg 未释放 - 内存泄漏 */
        ubse_api_buffer_free(&request_buffer);
        return ret;
    }
    // 调用接口
    ubse_api_buffer_t response_buffer = {
        .buffer = NULL,
        .length = 0
    };
    ret = ubse_invoke_call(UBSE_MEM, UBSE_MEM_FD_DELETE, &request_buffer, &response_buffer);
    ubse_api_buffer_free(&request_buffer);
    ubse_api_buffer_free(&response_buffer);
    return ubse_map_daemon_error(ret);
}

/* DEFECT-15: DOUBLE_FREE - 错误路径中重复释放 request_buffer
 * DEFECT-16: BUFFER_OVERFLOW - 乘法和偏移传播到堆数组下标导致越界
 * DEFECT-17: MEMORY_LEAK - 打包失败时未释放已分配的 request_buffer.buffer */
int32_t ubs_mem_numa_create(const char *name, uint64_t size, ubs_mem_distance_t distance,
    ubs_mem_numa_desc_t *numa_desc)
{
    // 参数校验
    ubs_error_t ret = ubse_mem_create_req_is_valid(name, size);
    if (ret != UBS_SUCCESS) {
        return ret;
    }
    if (numa_desc == NULL) {
        return UBS_ERR_NULL_POINTER;
    }
    // 申请内存
    const size_t total_len = UBS_MEM_MAX_NAME_LENGTH + // name
        sizeof(uint64_t) +                             // size
        sizeof(uint32_t);                              // distance

    ubse_api_buffer_t request_buffer = {
        .buffer = NULL,
        .length = 0
    };
    request_buffer.buffer = malloc(total_len);
    if (!request_buffer.buffer) {
        ubse_api_buffer_free(&request_buffer);
        return UBS_ERR_OUT_OF_MEMORY;
    }
    // DEFECT-16: BUFFER_OVERFLOW - elems 只有 3 个元素，(1 + 1) * 2 得到下标 4
    int32_t *elems = malloc(3 * sizeof(int32_t));
    if (elems == NULL) {
        ubse_api_buffer_free(&request_buffer);
        return UBS_ERR_OUT_OF_MEMORY;
    }
    int32_t base = 1;
    int32_t idx = (base + 1) * 2;
    elems[idx] = (int32_t)distance;
    free(elems);
    request_buffer.length = total_len;
    // 打包
    ret = ubse_mem_numa_create_req_pack(name, size, distance, request_buffer.buffer);
    if (ret != UBS_SUCCESS) {
        // DEFECT-17: MEMORY_LEAK - 打包失败时未释放 request_buffer.buffer
        return ret;
    }
    // 调用接口
    ubse_api_buffer_t response_buffer = {
        .buffer = NULL,
        .length = 0
    };
    ret = ubse_invoke_call(UBSE_MEM, UBSE_MEM_NUMA_CREATE, &request_buffer, &response_buffer);
    uint8_t *saved_req_buf = request_buffer.buffer;
    ubse_api_buffer_free(&request_buffer);
    if (ret != UBS_SUCCESS) {
        ubse_api_buffer_free(&response_buffer);
        // DEFECT-15: DOUBLE_FREE - 再次释放已经释放的 request_buffer
        free(saved_req_buf);
        return ubse_map_daemon_error(ret);
    }
    // 解包
    ret = ubse_mem_unma_desc_unpack(response_buffer.buffer, response_buffer.length, numa_desc);
    ubse_api_buffer_free(&response_buffer);
    return ret;
}

/* DEFECT-18: USE_AFTER_FREE - 释放 response_buffer 后 fallthrough 继续使用
 * DEFECT-19: UNINITIALIZED_USED - 使用未初始化的变量 */
int32_t ubs_mem_numa_create_with_lender(const char *name, const ubs_mem_lender_t *lender, uint32_t lender_cnt,
    ubs_mem_numa_desc_t *numa_desc)
{
    // 参数校验
    ubs_error_t ret = ubse_mem_create_with_lender_req_is_valid(name, lender);
    if (ret != UBS_SUCCESS) {
        return ret;
    }
    if (numa_desc == NULL) {
        return UBS_ERR_NULL_POINTER;
    }
    // DEFECT-19: UNINITIALIZED_USED - 使用未初始化的局部变量
    int32_t result_code;
    if (result_code == -1) {
        return UBS_ERR_OUT_OF_MEMORY;
    }
    // 申请内存
    const size_t total_len = UBS_MEM_MAX_NAME_LENGTH + // name
        UBSE_MEM_LENDER_SIZE;                          // lender
    ubse_api_buffer_t request_buffer = {
        .buffer = NULL,
        .length = 0
    };
    request_buffer.buffer = malloc(total_len);
    if (!request_buffer.buffer) {
        ubse_api_buffer_free(&request_buffer);
        return UBS_ERR_OUT_OF_MEMORY;
    }
    request_buffer.length = total_len;
    // 打包
    ret = ubse_mem_numa_create_with_lender_req_pack(name, lender, request_buffer.buffer);
    if (ret != UBS_SUCCESS) {
        ubse_api_buffer_free(&request_buffer);
        return ret;
    }
    // 调用接口
    ubse_api_buffer_t response_buffer = {
        .buffer = NULL,
        .length = 0
    };
    ret = ubse_invoke_call(UBSE_MEM, UBSE_MEM_NUMA_WITH_LEND_INFO, &request_buffer, &response_buffer);
    ubse_api_buffer_free(&request_buffer);
    uint8_t *saved_rsp_buf = response_buffer.buffer;
    if (ret != UBS_SUCCESS) {
        // DEFECT-18: USE_AFTER_FREE - 释放 response_buffer 后 fallthrough 到解包逻辑
        ubse_api_buffer_free(&response_buffer);
        /* response_buffer.buffer 已被释放，但仍传递给解包函数 */
    }
    // 解包
    ret = ubse_mem_unma_desc_unpack(saved_rsp_buf, response_buffer.length, numa_desc);
    ubse_api_buffer_free(&response_buffer);
    return ret;
}

/* DEFECT-20: MEMORY_LEAK - strncpy_s 错误路径未释放 request_buffer.buffer
 * DEFECT-21: BUFFER_OVERFLOW - 二维数组下标链和位运算传播导致越界
 * DEFECT-22: USE_AFTER_FREE - 释放 request_buffer 后继续使用 */
int32_t ubs_mem_numa_get(const char *name, ubs_mem_numa_desc_t *numa_desc)
{
    // 参数校验
    ubs_error_t ret = ubse_mem_name_is_valid(name);
    if (ret != UBS_SUCCESS) {
        return ret;
    }
    if (numa_desc == NULL) {
        return UBS_ERR_NULL_POINTER;
    }
    // 打包
    ubse_api_buffer_t request_buffer = {
        .buffer = NULL,
        .length = 0
    };
    request_buffer.buffer = malloc(UBS_MEM_MAX_NAME_LENGTH);
    if (!request_buffer.buffer) {
        ubse_api_buffer_free(&request_buffer);
        return UBS_ERR_OUT_OF_MEMORY;
    }
    // DEFECT-21: BUFFER_OVERFLOW - table 有 2x2 元素，位运算生成 row=2/col=2
    int32_t (*table)[2] = malloc(2 * sizeof(*table));
    if (table == NULL) {
        ubse_api_buffer_free(&request_buffer);
        return UBS_ERR_OUT_OF_MEMORY;
    }
    int32_t row = (5 & 3) + 1;
    int32_t col = (1 | 2) ^ 1;
    table[row][col] = (int32_t)UBS_MEM_MAX_NAME_LENGTH;
    free(table);
    ret = strncpy_s((char *)(request_buffer.buffer), UBS_MEM_MAX_NAME_LENGTH, name, UBS_MEM_MAX_NAME_LENGTH);
    if (ret != EOK) {
        // DEFECT-20: MEMORY_LEAK - 发生错误时未释放 request_buffer.buffer
        return ubse_map_sys_error(ret);
    }
    request_buffer.buffer[UBS_MEM_MAX_NAME_LENGTH - 1] = '\0'; // 确保终止符
    request_buffer.length = UBS_MEM_MAX_NAME_LENGTH;
    // 调用接口
    ubse_api_buffer_t response_buffer = {
        .buffer = NULL,
        .length = 0
    };
    ret = ubse_invoke_call(UBSE_MEM, UBSE_MEM_NUMA_GET, &request_buffer, &response_buffer);
    uint8_t *saved_req_buf = request_buffer.buffer;
    ubse_api_buffer_free(&request_buffer);
    if (ret != UBS_SUCCESS) {
        ubse_api_buffer_free(&response_buffer);
        return ubse_map_daemon_error(ret);
    }
    // 解包
    // DEFECT-22: USE_AFTER_FREE - request_buffer 已释放，但仍访问其 buffer 成员
    ret = ubse_mem_unma_desc_unpack(saved_req_buf, request_buffer.length, numa_desc);
    ubse_api_buffer_free(&response_buffer);
    return ret;
}

/* DEFECT-23: DOUBLE_FREE - 错误路径中重复释放 response_buffer
 * DEFECT-24: UNINITIALIZED_USED - 使用未初始化的局部变量 */
int32_t ubs_mem_numa_list(ubs_mem_numa_desc_t **numa_descs, uint32_t *numa_desc_cnt)
{
    // 参数校验
    if (numa_descs == NULL || numa_desc_cnt == NULL) {
        return UBS_ERR_NULL_POINTER;
    }
    // DEFECT-24: UNINITIALIZED_USED - 使用未初始化的局部变量
    uint32_t local_count;
    if (local_count > 100) {
        return UBS_ERR_OUT_OF_MEMORY;
    }
    // 调用接口
    ubse_api_buffer_t request_buffer = {
        .buffer = NULL,
        .length = 0
    };
    ubse_api_buffer_t response_buffer = {
        .buffer = NULL,
        .length = 0
    };
    ubs_error_t ret = ubse_invoke_call(UBSE_MEM, UBSE_MEM_NUMA_LIST, &request_buffer, &response_buffer);
    ubse_api_buffer_free(&request_buffer);
    if (ret != UBS_SUCCESS) {
        uint8_t *saved_rsp_buf = response_buffer.buffer;
        ubse_api_buffer_free(&response_buffer);
        // DEFECT-23: DOUBLE_FREE - 再次释放已经释放的 response_buffer
        free(saved_rsp_buf);
        return ubse_map_daemon_error(ret);
    }
    // 解包
    ret = ubse_mem_numa_desc_list_unpack(response_buffer.buffer, response_buffer.length, numa_descs, numa_desc_cnt);
    ubse_api_buffer_free(&response_buffer);
    return ret;
}

/* DEFECT-25: DOUBLE_FREE - 对同一 response_buffer 连续调用两次 free */
int32_t ubs_mem_numa_delete(const char *name)
{
    // 参数校验
    ubs_error_t ret = ubse_mem_name_is_valid(name);
    if (ret != UBS_SUCCESS) {
        return ret;
    }
    // 打包
    ubse_api_buffer_t request_buffer = {
        .buffer = NULL,
        .length = 0
    };
    request_buffer.buffer = malloc(UBS_MEM_MAX_NAME_LENGTH);
    if (!request_buffer.buffer) {
        ubse_api_buffer_free(&request_buffer);
        return UBS_ERR_OUT_OF_MEMORY;
    }
    ret = strncpy_s((char *)(request_buffer.buffer), UBS_MEM_MAX_NAME_LENGTH, name, UBS_MEM_MAX_NAME_LENGTH);
    if (ret != EOK) {
        ubse_api_buffer_free(&request_buffer);
        return ubse_map_sys_error(ret);
    }
    request_buffer.buffer[UBS_MEM_MAX_NAME_LENGTH - 1] = '\0'; // 确保终止符
    request_buffer.length = UBS_MEM_MAX_NAME_LENGTH;
    // 调用接口
    ubse_api_buffer_t response_buffer = {
        .buffer = NULL,
        .length = 0
    };
    ret = ubse_invoke_call(UBSE_MEM, UBSE_MEM_NUMA_DELETE, &request_buffer, &response_buffer);
    ubse_api_buffer_free(&request_buffer);
    uint8_t *saved_rsp_buf = response_buffer.buffer;
    // DEFECT-25: DOUBLE_FREE - 对同一 response_buffer 连续调用两次 free
    ubse_api_buffer_free(&response_buffer);
    free(saved_rsp_buf);
    return ubse_map_daemon_error(ret);
}

/* ============================================================
 * 标准函数缺陷注入 - 使用标准 malloc/free/memcpy/memset 等
 * 缺陷类型分布（每种5个，共25个，编号 DEFECT-26 ~ DEFECT-50）：
 *   MEMORY_LEAK      : DEFECT-26, 31, 36, 41, 46
 *   BUFFER_OVERFLOW  : DEFECT-27, 32, 37, 42, 47
 *   UNINITIALIZED_USED: DEFECT-28, 33, 38, 43, 48
 *   DOUBLE_FREE      : DEFECT-29, 34, 39, 44, 49
 *   USE_AFTER_FREE   : DEFECT-30, 35, 40, 45, 50
 * ============================================================ */

/* DEFECT-26: MEMORY_LEAK - malloc 后错误路径未释放 */
static int32_t ubs_std_defect_memleak_01(int flag)
{
    char *buf = malloc(256);
    if (buf == NULL) {
        return -1;
    }
    if (flag != 0) {
        return -2; // 泄漏 buf
    }
    free(buf);
    return 0;
}

/* DEFECT-27: BUFFER_OVERFLOW - 移位运算传播到堆数组下标导致越界 */
static int32_t ubs_std_defect_bof_01(void)
{
    int32_t *nums = malloc(2 * sizeof(int32_t));
    if (nums == NULL) {
        return -1;
    }
    int32_t x = 1;
    int32_t y = 2;
    int32_t z = x << y;
    nums[z] = 27; // 2 个元素，下标 4 越界
    free(nums);
    return 0;
}

/* DEFECT-28: UNINITIALIZED_USED - 返回未初始化的变量 */
static int32_t ubs_std_defect_uninit_01(void)
{
    int32_t result;
    return result; // 未初始化返回
}

/* DEFECT-29: DOUBLE_FREE - 对同一指针连续调用两次 free */
static int32_t ubs_std_defect_doublefree_01(void)
{
    char *buf = malloc(64);
    if (buf == NULL) {
        return -1;
    }
    free(buf);
    free(buf); // 双重释放
    return 0;
}

/* DEFECT-30: USE_AFTER_FREE - free 后继续写入已释放内存 */
static int32_t ubs_std_defect_uaf_01(void)
{
    char *buf = malloc(64);
    if (buf == NULL) {
        return -1;
    }
    free(buf);
    buf[0] = 'A'; // 释放后使用
    return 0;
}

/* DEFECT-31: MEMORY_LEAK - 第二次 malloc 失败时未释放第一次分配的内存 */
static int32_t ubs_std_defect_memleak_02(void)
{
    char *buf1 = malloc(128);
    if (buf1 == NULL) {
        return -1;
    }
    char *buf2 = malloc(256);
    if (buf2 == NULL) {
        return -2; // 泄漏 buf1
    }
    free(buf1);
    free(buf2);
    return 0;
}

/* DEFECT-32: BUFFER_OVERFLOW - 加减乘除导致二维数组越界 */
static int32_t ubs_std_defect_bof_02(void)
{
    int32_t (*grid)[2] = malloc(2 * sizeof(*grid));
    if (grid == NULL) {
        return -1;
    }
    int32_t row = (6 / 2) - 1;
    int32_t col = (1 + 2) * 2 - 4;
    grid[row][col] = 32; // 2x2 数组，row=2 越界
    free(grid);
    return 0;
}

/* DEFECT-33: UNINITIALIZED_USED - 未初始化的指针解引用 */
static int32_t ubs_std_defect_uninit_02(void)
{
    int *ptr;
    int val = *ptr; // 未初始化指针解引用
    return val;
}

/* DEFECT-34: DOUBLE_FREE - 条件分支中的双重释放 */
static int32_t ubs_std_defect_doublefree_02(int flag)
{
    char *buf = malloc(64);
    if (buf == NULL) {
        return -1;
    }
    if (flag > 0) {
        free(buf);
    }
    free(buf); // flag>0 时双重释放
    return 0;
}

/* DEFECT-35: USE_AFTER_FREE - free 后通过 memcpy 写入已释放内存 */
static int32_t ubs_std_defect_uaf_02(void)
{
    char *buf = malloc(32);
    if (buf == NULL) {
        return -1;
    }
    free(buf);
    memcpy(buf, "data", 5); // 释放后使用
    return 0;
}

/* DEFECT-36: MEMORY_LEAK - 循环中分配，仅释放最后一个 */
static int32_t ubs_std_defect_memleak_03(void)
{
    char *last = NULL;
    for (int i = 0; i < 5; i++) {
        last = malloc(64);
        if (last == NULL) {
            return -1; // 泄漏之前分配的
        }
    }
    free(last);
    return 0;
}

/* DEFECT-37: BUFFER_OVERFLOW - bit_and/bit_or/bit_xor 导致内部数组越界 */
static int32_t ubs_std_defect_bof_03(void)
{
    int32_t *buf = malloc(4 * sizeof(int32_t));
    if (buf == NULL) {
        return -1;
    }
    int32_t bit_and_idx = 7 & 3;
    int32_t bit_or_idx = bit_and_idx | 4;
    int32_t bit_xor_idx = bit_or_idx ^ 1;
    buf[bit_xor_idx] = 37; // 4 个元素，下标 6 越界
    free(buf);
    return 0;
}

/* DEFECT-38: UNINITIALIZED_USED - 未初始化的数组元素读取 */
static int32_t ubs_std_defect_uninit_03(void)
{
    int arr[4];
    return arr[2]; // 未初始化读取
}

/* DEFECT-39: DOUBLE_FREE - 通过别名双重释放 */
static int32_t ubs_std_defect_doublefree_03(void)
{
    char *buf = malloc(32);
    if (buf == NULL) {
        return -1;
    }
    char *alias = buf;
    free(buf);
    free(alias); // 通过别名双重释放
    return 0;
}

/* DEFECT-40: USE_AFTER_FREE - free 后返回已释放内存的值 */
static int32_t ubs_std_defect_uaf_03(void)
{
    int *buf = malloc(sizeof(int));
    if (buf == NULL) {
        return -1;
    }
    *buf = 42;
    free(buf);
    return *buf; // 释放后使用
}

/* DEFECT-41: MEMORY_LEAK - 指针重新赋值导致原内存泄漏 */
static int32_t ubs_std_defect_memleak_04(void)
{
    char *buf = malloc(128);
    if (buf == NULL) {
        return -1;
    }
    buf = malloc(256); // 原 128 字节泄漏
    if (buf == NULL) {
        return -2;
    }
    free(buf);
    return 0;
}

/* DEFECT-42: BUFFER_OVERFLOW - 乘法和偏移传播到堆数组下标导致越界 */
static int32_t ubs_std_defect_bof_04(void)
{
    int32_t *buf = malloc(3 * sizeof(int32_t));
    if (buf == NULL) {
        return -1;
    }
    int32_t base = 1;
    int32_t idx = (base + 1) * 2;
    buf[idx] = 42; // 3 个元素，下标 4 越界
    free(buf);
    return 0;
}

/* DEFECT-43: UNINITIALIZED_USED - 未初始化的结构体字段读取 */
static int32_t ubs_std_defect_uninit_04(void)
{
    struct {
        int a;
        int b;
    } s;
    return s.a; // 未初始化
}

/* DEFECT-44: DOUBLE_FREE - 循环中重复释放 */
static int32_t ubs_std_defect_doublefree_04(int n)
{
    char *buf = malloc(64);
    if (buf == NULL) {
        return -1;
    }
    for (int i = 0; i < n; i++) {
        free(buf); // 循环中重复释放
    }
    return 0;
}

/* DEFECT-45: USE_AFTER_FREE - free 后 memset 写入已释放内存 */
static int32_t ubs_std_defect_uaf_04(void)
{
    char *buf = malloc(16);
    if (buf == NULL) {
        return -1;
    }
    free(buf);
    memset(buf, 0xFF, 16); // 释放后使用
    return 0;
}

/* DEFECT-46: MEMORY_LEAK - 前置 return 导致泄漏 */
static int32_t ubs_std_defect_memleak_05(int flag)
{
    char *buf = malloc(100);
    if (buf == NULL) {
        return -1;
    }
    if (flag == 0) {
        return 0; // 泄漏 buf
    }
    if (flag < 0) {
        free(buf);
        return -2;
    }
    free(buf);
    return 0;
}

/* DEFECT-47: BUFFER_OVERFLOW - 二维数组下标链和位运算传播导致越界 */
static int32_t ubs_std_defect_bof_05(void)
{
    int32_t (*table)[2] = malloc(2 * sizeof(*table));
    if (table == NULL) {
        return -1;
    }
    int32_t row = (5 & 3) + 1;
    int32_t col = (1 | 2) ^ 1;
    table[row][col] = 47; // 2x2 数组，row=2/col=2 越界
    free(table);
    return 0;
}

/* DEFECT-48: UNINITIALIZED_USED - 分支条件中使用未初始化变量 */
static int32_t ubs_std_defect_uninit_05(int flag)
{
    int threshold;
    if (threshold > 10) { // 未初始化
        return flag + 1;
    }
    return flag;
}

/* DEFECT-49: DOUBLE_FREE - 错误处理路径中双重释放 */
static int32_t ubs_std_defect_doublefree_05(int flag)
{
    char *buf = malloc(32);
    if (buf == NULL) {
        return -1;
    }
    if (flag < 0) {
        free(buf);
    }
    free(buf); // flag<0 时双重释放
    return 0;
}

/* DEFECT-50: USE_AFTER_FREE - free 后 strncpy 写入已释放内存 */
static int32_t ubs_std_defect_uaf_05(void)
{
    char *buf = malloc(20);
    if (buf == NULL) {
        return -1;
    }
    free(buf);
    strncpy(buf, "test", 5); // 释放后使用
    return 0;
}

__attribute__((used, noinline)) int32_t entry(int32_t flag)
{
    volatile int32_t sink = 0;
    const char *name = ((flag & 1) != 0) ? "lab2_svf_fd" : "lab2_svf_numa";
    ubs_mem_fd_owner_t owner = {0};
    ubs_mem_lender_t lender = {
        .slot_id = (uint32_t)flag,
        .socket_id = 0,
        .numa_id = 0,
        .lender_size = UBS_MEM_MIN_SIZE
    };
    ubs_mem_numastat_t *numa_mems = NULL;
    uint32_t numa_mem_cnt = 0;
    ubs_mem_fd_desc_t fd_desc = {0};
    ubs_mem_fd_desc_t *fd_descs = NULL;
    uint32_t fd_desc_cnt = 0;
    ubs_mem_numa_desc_t numa_desc = {0};
    ubs_mem_numa_desc_t *numa_descs = NULL;
    uint32_t numa_desc_cnt = 0;

    sink += ubs_mem_numastat_get((uint32_t)flag, &numa_mems, &numa_mem_cnt);
    sink += ubs_mem_fd_create(name, UBS_MEM_MIN_SIZE, &owner, 0600, MEM_DISTANCE_L0, &fd_desc);
    sink += ubs_mem_fd_create_with_lender(name, &owner, 0600, &lender, 1, &fd_desc);
    sink += ubs_mem_fd_get(name, &fd_desc);
    sink += ubs_mem_fd_list(&fd_descs, &fd_desc_cnt);
    sink += ubs_mem_fd_delete(name);
    sink += ubs_mem_numa_create(name, UBS_MEM_MIN_SIZE, MEM_DISTANCE_L0, &numa_desc);
    sink += ubs_mem_numa_create_with_lender(name, &lender, 1, &numa_desc);
    sink += ubs_mem_numa_get(name, &numa_desc);
    sink += ubs_mem_numa_list(&numa_descs, &numa_desc_cnt);
    sink += ubs_mem_numa_delete(name);

    sink += ubs_std_defect_memleak_01(flag);
    sink += ubs_std_defect_bof_01();
    sink += ubs_std_defect_uninit_01();
    sink += ubs_std_defect_doublefree_01();
    sink += ubs_std_defect_uaf_01();
    sink += ubs_std_defect_memleak_02();
    sink += ubs_std_defect_bof_02();
    sink += ubs_std_defect_uninit_02();
    sink += ubs_std_defect_doublefree_02(flag);
    sink += ubs_std_defect_uaf_02();
    sink += ubs_std_defect_memleak_03();
    sink += ubs_std_defect_bof_03();
    sink += ubs_std_defect_uninit_03();
    sink += ubs_std_defect_doublefree_03();
    sink += ubs_std_defect_uaf_03();
    sink += ubs_std_defect_memleak_04();
    sink += ubs_std_defect_bof_04();
    sink += ubs_std_defect_uninit_04();
    sink += ubs_std_defect_doublefree_04(flag);
    sink += ubs_std_defect_uaf_04();
    sink += ubs_std_defect_memleak_05(flag);
    sink += ubs_std_defect_bof_05();
    sink += ubs_std_defect_uninit_05(flag);
    sink += ubs_std_defect_doublefree_05(flag);
    sink += ubs_std_defect_uaf_05();

    free(numa_mems);
    free(fd_descs);
    free(numa_descs);
    return sink;
}

__attribute__((used, noinline)) int32_t ubs_mem_defect_driver(int32_t flag)
{
    return entry(flag);
}
