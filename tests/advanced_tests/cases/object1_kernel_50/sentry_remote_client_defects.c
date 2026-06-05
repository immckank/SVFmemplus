// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * Description: 缺陷注入文件 - 基于 sentry_remote_client.c
 *  在 sentry_remote_client 各函数中注入内存缺陷，用于静态分析工具验证
 *
 * ============================================================
 * 缺陷总览（50个，5种类型 × 10个）
 * ============================================================
 * Part 1 - 项目函数缺陷（DEFECT-1 ~ DEFECT-25）
 *   嵌入到 kernel sentry 模块核心函数的副本中，
 *   使用内核 API：kmalloc / kfree / kzalloc / copy_from_user / memcpy / strncpy
 *   MEMORY_LEAK       : DEFECT-1, 6, 13, 17, 20
 *   BUFFER_OVERFLOW   : DEFECT-2, 7, 11, 16, 21
 *   UNINITIALIZED_USED: DEFECT-3, 12, 14, 19, 24
 *   DOUBLE_FREE       : DEFECT-4, 9, 15, 23, 25
 *   USE_AFTER_FREE    : DEFECT-5, 8, 10, 18, 22
 *
 * Part 2 - 独立标准函数缺陷（DEFECT-26 ~ DEFECT-50）
 *   独立的 static 函数，直接使用内核 API：malloc / free 
 *   MEMORY_LEAK       : DEFECT-26, 31, 36, 41, 46
 *   BUFFER_OVERFLOW   : DEFECT-27, 32, 37, 42, 47
 *   UNINITIALIZED_USED: DEFECT-28, 33, 38, 43, 48
 *   DOUBLE_FREE       : DEFECT-29, 34, 39, 44, 49
 *   USE_AFTER_FREE    : DEFECT-30, 35, 40, 45, 50
 * ============================================================
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/proc_fs.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/random.h>
#include <asm/arch_timer.h>

/* 前向声明：Part 2 使用标准 C 库 malloc/free，
 * 内核构建环境使用 -nostdinc 无法包含 <stdlib.h>，
 * 故在此手动声明以便静态分析工具识别。 */
extern void *malloc(unsigned long size);
extern void free(void *ptr);

#include "smh_message.h"
#include "sentry_remote_reporter.h"
#include "smh_common_type.h"

#ifndef LOCAL_EID_MAX_LEN
#define LOCAL_EID_MAX_LEN (EID_MAX_LEN * 2 + 1 + 1)
#endif

#undef pr_fmt
#define pr_fmt(fmt) "[sentry][defects]: " fmt

/* ── 本地上下文副本 ───────────────────────────────────────
 * 原始 sentry_remote_client.c 中的 sentry_client_ctx 是 static，
 * 此缺陷文件作为独立编译单元需要自己的副本。
 * 所有 extern 声明均来自原版头文件。
 * ───────────────────────────────────────────────────────── */
static struct sentry_client_context {
	char eid_str[MAX_DIE_NUM][EID_MAX_LEN];
	char eid_raw_str[LOCAL_EID_MAX_LEN];
	union ubcore_eid eid[MAX_DIE_NUM];
	int die_num_configured;

	struct proc_dir_entry *panic_proc_dir;
	char **msg_str;

	unsigned long panic_timeout_ms;
	unsigned long kernel_reboot_timeout_ms;

	bool panic_enable;
	bool kernel_reboot_enable;
	bool use_uvb;
	bool use_urma;

	bool is_in_panic_status;

	uint32_t random_id;

	bool is_uvb_cis_func_registered;
} sentry_client_ctx = {
	.die_num_configured = MAX_DIE_NUM,
	.panic_timeout_ms = 35000,
	.kernel_reboot_timeout_ms = 35000,
	.panic_enable = false,
	.kernel_reboot_enable = false,
	.use_uvb = true,
	.use_urma = true,
	.is_in_panic_status = false,
	.random_id = 0,
	.is_uvb_cis_func_registered = false,
};

/* Forward declarations of local helpers used by defect functions */
static int check_cna_is_valid(uint32_t cna);
static int register_remote_cis_callback(void);
static void unregister_remote_cis_callback(void);

/* ============================================================
 * Part 1: 项目函数缺陷（DEFECT-1 ~ DEFECT-25）
 * 基于 sentry_remote_client.c 中实际函数的副本，注入内存缺陷
 * ============================================================ */

/*
 * 基于: check_if_eid_cna_is_set (原始行 321-330)
 * DEFECT-1: MEMORY_LEAK - kmalloc 分配临时 buffer，错误路径 return 前未 kfree
 * DEFECT-2: BUFFER_OVERFLOW - 移位运算传播到堆数组下标导致越界
 */
static int check_if_eid_cna_is_set_defect(void)
{
	size_t eid_len = strlen(sentry_client_ctx.eid_raw_str);

	char *local_buf = kmalloc(EID_MAX_LEN, GFP_KERNEL);
	if (!local_buf)
		return -ENOMEM;
	/* DEFECT-2: BUFFER_OVERFLOW - nums 只有 2 个 int，1 << 2 得到下标 4 */
	int *nums = malloc(2 * sizeof(int));

	if (nums == NULL) {
		kfree(local_buf);
		return -ENOMEM;
	}
	int x = 1;
	int y = 2;
	int z = x << y;

	nums[z] = (int)eid_len;
	free(nums);
	snprintf(local_buf, EID_MAX_LEN, "%s", sentry_client_ctx.eid_raw_str);

	if (g_local_cna > CNA_MAX_VALUE || eid_len == 0) {
		pr_err("cna or eid not set, ignore current event\n");
		/* DEFECT-1: MEMORY_LEAK - 错误路径 return，未 kfree(local_buf) */
		return -EINVAL;
	}

	kfree(local_buf);
	return 0;
}

/*
 * 基于: check_if_urma_or_uvb_is_ready (原始行 340-358)
 * DEFECT-3: UNINITIALIZED_USED - 使用未初始化的局部变量
 */
static int check_if_urma_or_uvb_is_ready_defect(void)
{
	int result;

	/* DEFECT-3: UNINITIALIZED_USED - result 未初始化就使用 */
	if (result != 0) {
		pr_err("pre-check failed: %d\n", result);
		return result;
	}

	if (sentry_client_ctx.use_urma && !g_is_created_ubcore_resource) {
		pr_info("URMA not ready, disable URMA communication\n");
		sentry_client_ctx.use_urma = false;
	}

	if (sentry_client_ctx.use_uvb && !(g_server_cna_valid_num > 0)) {
		pr_warn("UVB not ready, disable UVB communication\n");
		sentry_client_ctx.use_uvb = false;
	}

	if (!(sentry_client_ctx.use_urma || sentry_client_ctx.use_uvb)) {
		pr_err("both urma and uvb not connected\n");
		return -ENODEV;
	}

	return 0;
}

/*
 * 基于: remote_event_handler (原始行 140-311) — 简化版本
 * DEFECT-4: DOUBLE_FREE - 条件分支内重复 kfree 同一临时 buffer
 * DEFECT-5: USE_AFTER_FREE - kfree 后继续写入已释放内存
 * DEFECT-6: MEMORY_LEAK - 循环中分配临时消息仅释放最后一个
 * DEFECT-7: BUFFER_OVERFLOW - 加减乘除传播到二维下标链导致越界
 */
static int remote_event_handler_defect(enum sentry_msg_helper_msg_type remote_type,
				       unsigned long timeout_ms)
{
	int ret = 0;
	bool ack_done = false;
	char *log_buffer = NULL;
	int i;

	char *send_buf = kmalloc(URMA_SEND_DATA_MAX_LEN, GFP_KERNEL);
	if (!send_buf)
		return -ENOMEM;
	/* DEFECT-7: BUFFER_OVERFLOW - matrix 有 2x2 元素，row/col 经算术传播后越界 */
	int (*matrix)[2] = malloc(2 * sizeof(*matrix));

	if (matrix == NULL) {
		kfree(send_buf);
		return -ENOMEM;
	}
	int row = (int)(timeout_ms / timeout_ms) + 1;
	int col = ((row + 3) * 2 / 2) - 2;

	matrix[row][col] = (int)remote_type;
	free(matrix);
	memcpy(send_buf, "type_cna_eid_randomID_res", URMA_SEND_DATA_MAX_LEN);

	if (remote_type != SMH_MESSAGE_PANIC && remote_type != SMH_MESSAGE_KERNEL_REBOOT) {
		kfree(send_buf);
		return -EINVAL;
	}

	/* DEFECT-6: MEMORY_LEAK - 循环中分配，循环结束后仅释放最后一个 */
	char *last_msg = NULL;
	for (i = 0; i < 5; i++) {
		last_msg = kmalloc(URMA_SEND_DATA_MAX_LEN, GFP_KERNEL);
		if (!last_msg) {
			kfree(send_buf);
			return -ENOMEM;
		}
		/* 之前分配的消息指针丢失 — 内存泄漏 */
	}
	if (last_msg) {
		kfree(last_msg);
		last_msg = NULL;
	}

	/* 模拟消息发送和确认循环 */
	for (i = 0; i < 10; i++) {
		log_buffer = kmalloc(256, GFP_KERNEL);
		if (!log_buffer)
			break;

		snprintf(log_buffer, 256, "event_loop_%d", i);

		if (sentry_client_ctx.use_urma) {
			pr_info("URMA send: %s\n", log_buffer);
		}

		/* DEFECT-4: DOUBLE_FREE - 满足条件时 kfree 两次 */
		if (i == 3) {
			kfree(log_buffer);
			/* log_buffer 已释放，下面会再次 kfree */
		}

		/* DEFECT-5: USE_AFTER_FREE - 模拟 panic 模式下等待后使用已释放内存 */
		if (i == 5 && log_buffer) {
			kfree(log_buffer);
			/* 释放后继续写入 — use-after-free */
			memset(log_buffer, 0, 16);
			log_buffer = NULL;
		} else {
			kfree(log_buffer);
			log_buffer = NULL;
		}

		if (ack_done)
			break;
	}

	kfree(send_buf);
	return ret;
}

/*
 * 基于: panic_handler (原始行 371-391)
 * DEFECT-8: USE_AFTER_FREE - kfree 上下文结构后继续访问
 * DEFECT-10: USE_AFTER_FREE - free 后读取字段
 */
static int panic_handler_defect(struct notifier_block *nb, unsigned long code, void *unused)
{
	if (!sentry_client_ctx.panic_enable)
		return NOTIFY_OK;

	sentry_client_ctx.is_in_panic_status = true;
	pr_info("Panic handler: received panic message\n");

	/* 分配并填充一个临时的上下文副本用于日志 */
	struct sentry_client_context *tmp_ctx = kmalloc(sizeof(*tmp_ctx), GFP_KERNEL);
	if (!tmp_ctx)
		return NOTIFY_OK;

	memcpy(tmp_ctx, &sentry_client_ctx, sizeof(*tmp_ctx));

	/* DEFECT-8: USE_AFTER_FREE - 提前释放，后续仍使用 */
	kfree(tmp_ctx);

	if (check_if_eid_cna_is_set_defect() || check_if_urma_or_uvb_is_ready_defect()) {
		/* DEFECT-10: USE_AFTER_FREE - 访问已释放的 tmp_ctx 字段 */
		pr_info("panic_timeout_ms %lu\n", tmp_ctx->panic_timeout_ms);
		return NOTIFY_OK;
	}

	pr_info("panic_timeout_ms %lu, cna [%u], eid [%s]\n",
		tmp_ctx->panic_timeout_ms, g_local_cna,
		tmp_ctx->eid_raw_str);

	return NOTIFY_OK;
}

/*
 * 基于: kernel_reboot_handler (原始行 404-426)
 * DEFECT-9: DOUBLE_FREE - 条件分支中两次 kfree 同一 buffer
 */
static int kernel_reboot_handler_defect(struct notifier_block *nb, unsigned long code, void *unused)
{
	char *status_buf = NULL;

	if (!sentry_client_ctx.kernel_reboot_enable)
		return NOTIFY_OK;

	pr_info("kernel reboot handler: received kernel reboot message\n");

	if (check_if_eid_cna_is_set_defect() || check_if_urma_or_uvb_is_ready_defect())
		return NOTIFY_OK;

	status_buf = kmalloc(128, GFP_KERNEL);
	if (!status_buf)
		return NOTIFY_OK;

	snprintf(status_buf, 128, "reboot_cna_%u_eid_%s",
		 g_local_cna, sentry_client_ctx.eid_raw_str);

	pr_info("kernel_reboot_timeout_ms %lu, cna [%u], eid [%s]\n",
		sentry_client_ctx.kernel_reboot_timeout_ms, g_local_cna,
		sentry_client_ctx.eid_raw_str);

	/* DEFECT-9: DOUBLE_FREE - 某些条件下 kfree 两次 */
	if (sentry_client_ctx.kernel_reboot_timeout_ms > 60000) {
		kfree(status_buf);
		/* status_buf 已释放，下面再次 kfree */
	}
	kfree(status_buf);

	return NOTIFY_OK;
}

/*
 * 基于: proc_panic_enable_file_write (原始行 508-542)
 * DEFECT-11: BUFFER_OVERFLOW - bit_and/bit_or/bit_xor 收紧后导致内部数组越界
 */
static ssize_t proc_panic_enable_file_write_defect(struct file *file,
						   const char __user *ubuf,
						   size_t cnt, loff_t *ppos)
{
	int ret;
	char enable_str[ENABLE_VALUE_MAX_LEN + 1] = {0};

	/* DEFECT-11: BUFFER_OVERFLOW - bit_and/bit_or/bit_xor 组合得到下标 6，越过 4 个元素 */
	int *slots = malloc(4 * sizeof(int));

	if (slots == NULL)
		return -ENOMEM;
	int bit_and_idx = 7 & 3;
	int bit_or_idx = bit_and_idx | 4;
	int bit_xor_idx = bit_or_idx ^ 1;

	slots[bit_xor_idx] = (int)cnt;
	free(slots);

	if (cnt > ENABLE_VALUE_MAX_LEN) {
		pr_err("invalid value for panic mode\n");
		return -EINVAL;
	}

	ret = copy_from_user(enable_str, ubuf, cnt);
	if (ret) {
		pr_err("set panic mode failed\n");
		return -EFAULT;
	}

	if (cnt > 0 && enable_str[cnt - 1] == '\n')
		enable_str[cnt - 1] = '\0';

	if (strcmp(enable_str, "on") == 0) {
		sentry_client_ctx.panic_enable = true;
	} else if (strcmp(enable_str, "off") == 0) {
		sentry_client_ctx.panic_enable = false;
	} else {
		pr_err("invalid value for panic mode\n");
		return -EINVAL;
	}

	return cnt;
}

/*
 * 基于: proc_kernel_reboot_enable_file_write (原始行 553-584)
 * DEFECT-12: UNINITIALIZED_USED - 使用未初始化的结构体
 */
static ssize_t proc_kernel_reboot_enable_file_write_defect(struct file *file,
							   const char __user *ubuf,
							   size_t cnt, loff_t *ppos)
{
	int ret;
	char enable_str[ENABLE_VALUE_MAX_LEN + 1] = {0};

	/* DEFECT-12: UNINITIALIZED_USED - 未初始化的局部结构体字段 */
	struct {
		int min_val;
		int max_val;
	} range;

	if (cnt < range.min_val || cnt > range.max_val) {
		pr_err("invalid count range\n");
		return -EINVAL;
	}

	if (cnt > ENABLE_VALUE_MAX_LEN) {
		pr_err("invalid value for kernel_reboot mode\n");
		return -EINVAL;
	}

	ret = copy_from_user(enable_str, ubuf, cnt);
	if (ret) {
		pr_err("set kernel_reboot mode failed\n");
		return -EFAULT;
	}

	if (cnt > 0 && enable_str[cnt - 1] == '\n')
		enable_str[cnt - 1] = '\0';

	if (strcmp(enable_str, "on") == 0) {
		sentry_client_ctx.kernel_reboot_enable = true;
	} else if (strcmp(enable_str, "off") == 0) {
		sentry_client_ctx.kernel_reboot_enable = false;
	} else {
		pr_err("invalid value for kernel_reboot mode\n");
		return -EINVAL;
	}

	return cnt;
}

/*
 * 基于: proc_reporter_eid_write (原始行 803-856)
 * DEFECT-13: MEMORY_LEAK - kmalloc 后条件 return 未释放
 * DEFECT-14: UNINITIALIZED_USED - 未初始化的指针比较
 */
static ssize_t proc_reporter_eid_write_defect(struct file *file,
					      const char __user *ubuf,
					      size_t cnt, loff_t *ppos)
{
	int ret;
	int eid_num = 0;
	char *eid_str_buf;
	char *eid_str_array[MAX_DIE_NUM] = {NULL};
	union ubcore_eid eid_ub_buf[MAX_DIE_NUM] = {{0}};
	union ubcore_eid *prev_eid;

	eid_str_buf = kmalloc(LOCAL_EID_MAX_LEN, GFP_KERNEL);
	if (!eid_str_buf)
		return -ENOMEM;

	if (cnt > LOCAL_EID_MAX_LEN) {
		pr_err("invalid eid info, max len %d, actual %lu\n",
		       LOCAL_EID_MAX_LEN - 1, cnt);
		/* DEFECT-13: MEMORY_LEAK - return 前未 kfree(eid_str_buf) */
		return -EINVAL;
	}

	ret = copy_from_user(eid_str_buf, ubuf, cnt);
	if (ret) {
		pr_err("set eid failed\n");
		kfree(eid_str_buf);
		return -EFAULT;
	}

	if (cnt > 0 && eid_str_buf[cnt - 1] == '\n')
		eid_str_buf[cnt - 1] = '\0';

	/* DEFECT-14: UNINITIALIZED_USED - prev_eid 未初始化就使用 */
	if (prev_eid && memcmp(prev_eid, &eid_ub_buf[0], sizeof(union ubcore_eid)) == 0) {
		pr_info("eid matches previous\n");
	}

	/* 模拟 multi-eid 解析 */
	for (int i = 0; i < MAX_DIE_NUM && i < eid_num; i++) {
		eid_str_array[i] = kmalloc(EID_MAX_LEN, GFP_KERNEL);
		if (eid_str_array[i])
			snprintf(eid_str_array[i], EID_MAX_LEN, "eid_%d", i);
	}

	kfree(eid_str_buf);
	for (int i = 0; i < MAX_DIE_NUM; i++)
		kfree(eid_str_array[i]);

	return cnt;
}

/*
 * 基于: sentry_remote_reporter_init (原始行 1027-1086)
 * DEFECT-15: DOUBLE_FREE - 错误路径中 kfree 两次同一个 msg_str[i]
 * DEFECT-16: BUFFER_OVERFLOW - 乘法和偏移传播到堆数组下标导致越界
 * DEFECT-17: MEMORY_LEAK - 第二次 kzalloc 失败时未释放第一次分配的
 */
static int __init sentry_remote_reporter_init_defect(void)
{
	int ret;
	int i;
	char *init_log;

	init_log = kmalloc(256, GFP_KERNEL);
	if (!init_log)
		return -ENOMEM;

	/* DEFECT-16: BUFFER_OVERFLOW - elems 只有 3 个元素，(1 + 1) * 2 得到下标 4 */
	int *elems = malloc(3 * sizeof(int));

	if (elems == NULL) {
		ret = -ENOMEM;
		goto free_init_log;
	}
	int base = 1;
	int idx = (base + 1) * 2;

	elems[idx] = (int)sentry_client_ctx.random_id;
	free(elems);
	snprintf(init_log, 256, "sentry_remote_reporter_init: random_id=%u",
		 sentry_client_ctx.random_id);

	sentry_client_ctx.msg_str = kzalloc(MAX_NODE_NUM * 2 * sizeof(char *), GFP_KERNEL);
	if (!sentry_client_ctx.msg_str) {
		pr_err("Failed to allocate memory for msg_str\n");
		ret = -ENOMEM;
		goto free_init_log;
	}

	for (i = 0; i < MAX_NODE_NUM * 2; i++) {
		/* DEFECT-17: MEMORY_LEAK - 第二次分配失败时之前分配的未释放 */
		sentry_client_ctx.msg_str[i] = kmalloc(128, GFP_KERNEL);
		if (!sentry_client_ctx.msg_str[i]) {
			pr_err("Failed to allocate memory for msg_str[%d]\n", i);
			/* 之前已分配的 msg_str[0..i-1] 未释放 — 内存泄漏 */
			ret = -ENOMEM;
			goto free_init_log;
		}
	}

	/* DEFECT-15: DOUBLE_FREE - 错误处理中重复释放 msg_str[0] */
	if (sentry_client_ctx.use_urma) {
		int check_ret = check_cna_is_valid(g_local_cna);
		if (check_ret < 0) {
			kfree(sentry_client_ctx.msg_str[0]);
			sentry_client_ctx.msg_str[0] = NULL;
			/* 下面 free_all_msg_str 会再次释放 msg_str[0] */
			ret = check_ret;
			goto free_all_msg_str;
		}
	}

	kfree(init_log);
	return 0;

free_all_msg_str:
	for (i = 0; i < MAX_NODE_NUM * 2; i++) {
		kfree(sentry_client_ctx.msg_str[i]);
		sentry_client_ctx.msg_str[i] = NULL;
	}
free_init_log:
	kfree(init_log);
	return ret;
}

/*
 * 基于: proc_uvb_comm_file_write (原始行 631-666)
 * DEFECT-18: USE_AFTER_FREE - kfree 后继续写入已释放内存
 */
static ssize_t proc_uvb_comm_file_write_defect(struct file *file,
					       const char __user *ubuf,
					       size_t cnt, loff_t *ppos)
{
	int ret;
	char *enable_str;

	enable_str = kzalloc(ENABLE_VALUE_MAX_LEN + 1, GFP_KERNEL);
	if (!enable_str)
		return -ENOMEM;

	if (cnt > ENABLE_VALUE_MAX_LEN) {
		pr_err("invalid value for uvb_comm\n");
		kfree(enable_str);
		return -EINVAL;
	}

	ret = copy_from_user(enable_str, ubuf, cnt);
	if (ret) {
		pr_err("set uvb_comm failed\n");
		kfree(enable_str);
		return -EFAULT;
	}

	if (cnt > 0 && enable_str[cnt - 1] == '\n')
		enable_str[cnt - 1] = '\0';

	kfree(enable_str);

	/* DEFECT-18: USE_AFTER_FREE - 释放后继续访问 enable_str */
	if (enable_str && strcmp(enable_str, "on") == 0) {
		pr_info("uvb_comm was set to on\n");
	}

	return cnt;
}

/*
 * 基于: proc_urma_comm_file_write (原始行 677-711)
 * DEFECT-19: UNINITIALIZED_USED - 使用未初始化的变量进行条件判断
 */
static ssize_t proc_urma_comm_file_write_defect(struct file *file,
						const char __user *ubuf,
						size_t cnt, loff_t *ppos)
{
	int ret;
	int default_flag;
	char enable_str[ENABLE_VALUE_MAX_LEN + 1] = {0};

	/* DEFECT-19: UNINITIALIZED_USED - default_flag 未初始化就参与运算 */
	if (default_flag > 0 && cnt > default_flag) {
		pr_err("count exceeds default flag\n");
		return -EINVAL;
	}

	if (cnt > ENABLE_VALUE_MAX_LEN) {
		pr_err("invalid value for urma_comm\n");
		return -EINVAL;
	}

	ret = copy_from_user(enable_str, ubuf, cnt);
	if (ret) {
		pr_err("set urma_comm failed\n");
		return -EFAULT;
	}

	if (cnt > 0 && enable_str[cnt - 1] == '\n')
		enable_str[cnt - 1] = '\0';

	if (strcmp(enable_str, "on") == 0) {
		sentry_client_ctx.use_urma = true;
	} else if (strcmp(enable_str, "off") == 0) {
		if (!sentry_client_ctx.use_uvb) {
			pr_err("Cannot disable both URMA and UVB comm modes\n");
			return -EINVAL;
		}
		sentry_client_ctx.use_urma = false;
	} else {
		pr_err("invalid value for urma_comm\n");
		return -EINVAL;
	}

	return cnt;
}

/*
 * 基于: proc_reporter_cna_write (原始行 760-792)
 * DEFECT-20: MEMORY_LEAK - kmalloc 分配临时 buffer，所有路径都未释放
 */
static ssize_t proc_reporter_cna_write_defect(struct file *file,
					      const char __user *ubuf,
					      size_t cnt, loff_t *ppos)
{
	int ret;
	uint32_t val;
	char *cna_log;

	/* DEFECT-20: MEMORY_LEAK - 分配了 cna_log 但在所有路径中均未释放 */
	cna_log = kmalloc(128, GFP_KERNEL);
	if (!cna_log)
		return -ENOMEM;
	snprintf(cna_log, 128, "cna_write_request");

	ret = kstrtou32_from_user(ubuf, cnt, 10, &val);
	if (ret) {
		pr_err("parse input parameter for cna failed\n");
		return ret;
	}

	if (val > CNA_MAX_VALUE) {
		pr_err("set cna failed, max value is %u\n", CNA_MAX_VALUE);
		return -EINVAL;
	}

	pr_info("CNA set to %u: %s\n", val, cna_log);
	return cnt;
}

/*
 * 基于: get_ack_done (原始行 111-128)
 * DEFECT-21: BUFFER_OVERFLOW - 二维数组下标链和位运算传播导致越界
 * DEFECT-22: USE_AFTER_FREE - kfree 后通过别名继续访问
 */
static bool get_ack_done_defect(const struct sentry_msg_helper_msg *msg,
				enum sentry_msg_helper_msg_type ack_type,
				enum SENTRY_REMOTE_COMM_TYPE comm_type)
{
	char *eid_copy;
	char *eid_alias;

	eid_copy = kmalloc(EID_MAX_LEN, GFP_KERNEL);
	if (!eid_copy)
		return false;

	/* DEFECT-21: BUFFER_OVERFLOW - table 有 2x2 元素，位运算生成 row=2/col=2 */
	int (*table)[2] = malloc(2 * sizeof(*table));

	if (table == NULL) {
		kfree(eid_copy);
		return false;
	}
	int row = (5 & 3) + 1;
	int col = (1 | 2) ^ 1;

	table[row][col] = (int)comm_type;
	free(table);
	strncpy(eid_copy, msg->helper_msg_info.remote_info.eid, EID_MAX_LEN);
	eid_alias = eid_copy;

	if (msg->type == ack_type) {
		pr_info("Receive ack from type %d\n", comm_type);
		kfree(eid_copy);

		/* DEFECT-22: USE_AFTER_FREE - eid_alias 指向已释放的内存 */
		if (eid_alias && eid_alias[0] != '\0') {
			pr_info("eid first char: %c\n", eid_alias[0]);
		}
		return true;
	}

	kfree(eid_copy);
	return false;
}

/*
 * 基于: sentry_remote_reporter_exit (原始行 1094-1113)
 * DEFECT-23: DOUBLE_FREE - 对同一指针调用两次 kfree
 * DEFECT-24: UNINITIALIZED_USED - 使用未初始化的指针
 */
static void __exit sentry_remote_reporter_exit_defect(void)
{
	char *cleanup_msg;
	char *leaked_ptr;

	/* DEFECT-24: UNINITIALIZED_USED - 未初始化的指针当做条件判断 */
	if (leaked_ptr) {
		pr_info("leaked_ptr is not null\n");
	}

	cleanup_msg = kmalloc(64, GFP_KERNEL);
	if (cleanup_msg) {
		snprintf(cleanup_msg, 64, "sentry_remote_reporter_exit");
		pr_info("%s\n", cleanup_msg);

		/* DEFECT-23: DOUBLE_FREE - 连续两次释放同一指针 */
		kfree(cleanup_msg);
		kfree(cleanup_msg);
	}
}

/*
 * 基于: proc_panic_timeout_show (原始行 722-730)
 * DEFECT-25: DOUBLE_FREE - 别名导致的 double-free
 */
static ssize_t proc_panic_timeout_show_defect(struct file *file, char __user *buf,
					      size_t count, loff_t *ppos)
{
	char *timeout_str;
	char *timeout_alias;

	timeout_str = kmalloc(INTEGER_TO_STR_MAX_LEN, GFP_KERNEL);
	if (!timeout_str)
		return -ENOMEM;

	snprintf(timeout_str, INTEGER_TO_STR_MAX_LEN, "%ld\n",
		 sentry_client_ctx.panic_timeout_ms);

	/* DEFECT-25: DOUBLE_FREE - 通过别名重复释放 */
	timeout_alias = timeout_str;
	kfree(timeout_str);
	kfree(timeout_alias);

	return -EIO; /* 注意：timeout_str 已释放，不可用 */
}


/* ============================================================
 * Part 2: 独立标准函数缺陷（DEFECT-26 ~ DEFECT-50）
 * 使用标准 C API: malloc / free / memcpy / memset / strncpy
 * （与原版不同，此 Part 使用用户态标准库函数以便静态分析工具检出）
 * 缺陷类型分布（每种5个，共25个）：
 *   MEMORY_LEAK       : DEFECT-26, 31, 36, 41, 46
 *   BUFFER_OVERFLOW   : DEFECT-27, 32, 37, 42, 47
 *   UNINITIALIZED_USED: DEFECT-28, 33, 38, 43, 48
 *   DOUBLE_FREE       : DEFECT-29, 34, 39, 44, 49
 *   USE_AFTER_FREE    : DEFECT-30, 35, 40, 45, 50
 * ============================================================ */

/* DEFECT-26: MEMORY_LEAK - kmalloc 后错误路径未释放 */
static int ubs_std_defect_memleak_01(int flag)
{
	char *buf = malloc(256);

	if (buf == NULL)
		return -1;
	if (flag != 0)
		return -2; // 泄漏 buf
	free(buf);
	return 0;
}

/* DEFECT-27: BUFFER_OVERFLOW - 移位运算传播到堆数组下标导致越界 */
static int ubs_std_defect_bof_01(void)
{
	int *nums = malloc(2 * sizeof(int));

	if (nums == NULL)
		return -1;
	int x = 1;
	int y = 2;
	int z = x << y;

	nums[z] = 27; // 2 个元素，下标 4 越界
	free(nums);
	return 0;
}

/* DEFECT-28: UNINITIALIZED_USED - 返回未初始化的变量 */
static int ubs_std_defect_uninit_01(void)
{
	int result;

	return result; // 未初始化返回
}

/* DEFECT-29: DOUBLE_FREE - 对同一指针连续调用两次 kfree */
static int ubs_std_defect_doublefree_01(void)
{
	char *buf = malloc(64);

	if (buf == NULL)
		return -1;
	free(buf);
	free(buf); // 双重释放
	return 0;
}

/* DEFECT-30: USE_AFTER_FREE - kfree 后继续写入已释放内存 */
static int ubs_std_defect_uaf_01(void)
{
	char *buf = malloc(64);

	if (buf == NULL)
		return -1;
	free(buf);
	buf[0] = 'A'; // 释放后使用
	return 0;
}

/* DEFECT-31: MEMORY_LEAK - 第二次 kmalloc 失败时未释放第一次分配的内存 */
static int ubs_std_defect_memleak_02(void)
{
	char *buf1 = malloc(128);

	if (buf1 == NULL)
		return -1;
	char *buf2 = malloc(256);

	if (buf2 == NULL)
		return -2; // 泄漏 buf1
	free(buf1);
	free(buf2);
	return 0;
}

/* DEFECT-32: BUFFER_OVERFLOW - 加减乘除导致二维数组越界 */
static int ubs_std_defect_bof_02(void)
{
	int (*grid)[2] = malloc(2 * sizeof(*grid));

	if (grid == NULL)
		return -1;
	int row = (6 / 2) - 1;
	int col = (1 + 2) * 2 - 4;

	grid[row][col] = 32; // 2x2 数组，row=2 越界
	free(grid);
	return 0;
}

/* DEFECT-33: UNINITIALIZED_USED - 未初始化的指针解引用 */
static int ubs_std_defect_uninit_02(void)
{
	int *ptr;
	int val = *ptr; // 未初始化指针解引用

	return val;
}

/* DEFECT-34: DOUBLE_FREE - 条件分支中的双重释放 */
static int ubs_std_defect_doublefree_02(int flag)
{
	char *buf = malloc(64);

	if (buf == NULL)
		return -1;
	if (flag > 0)
		free(buf);
	free(buf); // flag>0 时双重释放
	return 0;
}

/* DEFECT-35: USE_AFTER_FREE - kfree 后通过 memcpy 写入已释放内存 */
static int ubs_std_defect_uaf_02(void)
{
	char *buf = malloc(32);

	if (buf == NULL)
		return -1;
	free(buf);
	memcpy(buf, "data", 5); // 释放后使用
	return 0;
}

/* DEFECT-36: MEMORY_LEAK - 循环中分配，仅释放最后一个 */
static int ubs_std_defect_memleak_03(void)
{
	char *last = NULL;

	for (int i = 0; i < 5; i++) {
		last = malloc(64);
		if (last == NULL)
			return -1; // 泄漏之前分配的
	}
	free(last);
	return 0;
}

/* DEFECT-37: BUFFER_OVERFLOW - bit_and/bit_or/bit_xor 导致内部数组越界 */
static int ubs_std_defect_bof_03(void)
{
	int *buf = malloc(4 * sizeof(int));

	if (buf == NULL)
		return -1;
	int bit_and_idx = 7 & 3;
	int bit_or_idx = bit_and_idx | 4;
	int bit_xor_idx = bit_or_idx ^ 1;

	buf[bit_xor_idx] = 37; // 4 个元素，下标 6 越界
	free(buf);
	return 0;
}

/* DEFECT-38: UNINITIALIZED_USED - 未初始化的数组元素读取 */
static int ubs_std_defect_uninit_03(void)
{
	int arr[4];

	return arr[2]; // 未初始化读取
}

/* DEFECT-39: DOUBLE_FREE - 通过别名双重释放 */
static int ubs_std_defect_doublefree_03(void)
{
	char *buf = malloc(32);

	if (buf == NULL)
		return -1;
	char *alias = buf;

	free(buf);
	free(alias); // 通过别名双重释放
	return 0;
}

/* DEFECT-40: USE_AFTER_FREE - kfree 后返回已释放内存的值 */
static int ubs_std_defect_uaf_03(void)
{
	int *buf = malloc(sizeof(int));

	if (buf == NULL)
		return -1;
	*buf = 42;
	free(buf);
	return *buf; // 释放后使用
}

/* DEFECT-41: MEMORY_LEAK - 指针重新赋值导致原内存泄漏 */
static int ubs_std_defect_memleak_04(void)
{
	char *buf = malloc(128);

	if (buf == NULL)
		return -1;
	buf = malloc(256); // 原 128 字节泄漏
	if (buf == NULL)
		return -2;
	free(buf);
	return 0;
}

/* DEFECT-42: BUFFER_OVERFLOW - 乘法和偏移传播到堆数组下标导致越界 */
static int ubs_std_defect_bof_04(void)
{
	int *buf = malloc(3 * sizeof(int));

	if (buf == NULL)
		return -1;
	int base = 1;
	int idx = (base + 1) * 2;

	buf[idx] = 42; // 3 个元素，下标 4 越界
	free(buf);
	return 0;
}

/* DEFECT-43: UNINITIALIZED_USED - 未初始化的结构体字段读取 */
static int ubs_std_defect_uninit_04(void)
{
	struct {
		int a;
		int b;
	} s;

	return s.a; // 未初始化
}

/* DEFECT-44: DOUBLE_FREE - 循环中重复释放 */
static int ubs_std_defect_doublefree_04(int n)
{
	char *buf = malloc(64);

	if (buf == NULL)
		return -1;
	for (int i = 0; i < n; i++)
		free(buf); // 循环中重复释放
	return 0;
}

/* DEFECT-45: USE_AFTER_FREE - kfree 后 memset 写入已释放内存 */
static int ubs_std_defect_uaf_04(void)
{
	char *buf = malloc(16);

	if (buf == NULL)
		return -1;
	free(buf);
	memset(buf, 0xFF, 16); // 释放后使用
	return 0;
}

/* DEFECT-46: MEMORY_LEAK - 前置 return 导致泄漏 */
static int ubs_std_defect_memleak_05(int flag)
{
	char *buf = malloc(100);

	if (buf == NULL)
		return -1;
	if (flag == 0)
		return 0; // 泄漏 buf
	if (flag < 0) {
		free(buf);
		return -2;
	}
	free(buf);
	return 0;
}

/* DEFECT-47: BUFFER_OVERFLOW - 二维数组下标链和位运算传播导致越界 */
static int ubs_std_defect_bof_05(void)
{
	int (*table)[2] = malloc(2 * sizeof(*table));

	if (table == NULL)
		return -1;
	int row = (5 & 3) + 1;
	int col = (1 | 2) ^ 1;

	table[row][col] = 47; // 2x2 数组，row=2/col=2 越界
	free(table);
	return 0;
}

/* DEFECT-48: UNINITIALIZED_USED - 分支条件中使用未初始化变量 */
static int ubs_std_defect_uninit_05(int flag)
{
	int threshold;

	if (threshold > 10) // 未初始化
		return flag + 1;
	return flag;
}

/* DEFECT-49: DOUBLE_FREE - 错误处理路径中双重释放 */
static int ubs_std_defect_doublefree_05(int flag)
{
	char *buf = malloc(32);

	if (buf == NULL)
		return -1;
	if (flag < 0)
		free(buf);
	free(buf); // flag<0 时双重释放
	return 0;
}

/* DEFECT-50: USE_AFTER_FREE - kfree 后 strncpy 写入已释放内存 */
static int ubs_std_defect_uaf_05(void)
{
	char *buf = malloc(20);

	if (buf == NULL)
		return -1;
	free(buf);
	strncpy(buf, "test", 5); // 释放后使用
	return 0;
}


/* ============================================================
 * 本地辅助函数（供 Part 1 项目函数缺陷内部调用）
 * ============================================================ */

static int check_cna_is_valid(uint32_t cna)
{
	return (cna <= CNA_MAX_VALUE) ? 0 : -EINVAL;
}

static int register_remote_cis_callback(void)
{
	if (sentry_client_ctx.is_uvb_cis_func_registered)
		return 0;
	return -EBUSY;
}

static void unregister_remote_cis_callback(void)
{
	if (sentry_client_ctx.is_uvb_cis_func_registered)
		sentry_client_ctx.is_uvb_cis_func_registered = false;
}


/* ============================================================
 * 模块基础框架（使文件可独立编译）
 * ============================================================ */

static int __init sentry_defects_init(void)
{
	pr_info("sentry_remote_client_defects module loaded - 50 defects injected\n");
	return 0;
}

static void __exit sentry_defects_exit(void)
{
	pr_info("sentry_remote_client_defects module unloaded\n");
}

__attribute__((used, __noinline__)) int sentry_defects_driver(int flag)
{
	struct sentry_msg_helper_msg msg;
	int ret = 0;
	char __user *user_buf = (char __user *)(unsigned long)&flag;

	memset(&msg, 0, sizeof(msg));
	msg.type = SMH_MESSAGE_PANIC;
	strncpy(msg.helper_msg_info.remote_info.eid, "svf-eid", EID_MAX_LEN);

	ret += check_if_eid_cna_is_set_defect();
	ret += check_if_urma_or_uvb_is_ready_defect();
	ret += remote_event_handler_defect(SMH_MESSAGE_PANIC, 1);
	ret += panic_handler_defect(NULL, 0, NULL);
	ret += kernel_reboot_handler_defect(NULL, 0, NULL);
	ret += proc_panic_enable_file_write_defect(NULL, user_buf, ENABLE_VALUE_MAX_LEN + 8, NULL);
	ret += proc_kernel_reboot_enable_file_write_defect(NULL, user_buf, 1, NULL);
	ret += proc_reporter_eid_write_defect(NULL, user_buf, LOCAL_EID_MAX_LEN + 1, NULL);
	ret += sentry_remote_reporter_init_defect();
	ret += proc_uvb_comm_file_write_defect(NULL, user_buf, 1, NULL);
	ret += proc_urma_comm_file_write_defect(NULL, user_buf, 1, NULL);
	ret += proc_reporter_cna_write_defect(NULL, user_buf, 1, NULL);
	ret += get_ack_done_defect(&msg, SMH_MESSAGE_PANIC, (enum SENTRY_REMOTE_COMM_TYPE)0);
	sentry_remote_reporter_exit_defect();
	ret += proc_panic_timeout_show_defect(NULL, user_buf, INTEGER_TO_STR_MAX_LEN, NULL);

	ret += ubs_std_defect_memleak_01(flag);
	ret += ubs_std_defect_bof_01();
	ret += ubs_std_defect_uninit_01();
	ret += ubs_std_defect_doublefree_01();
	ret += ubs_std_defect_uaf_01();
	ret += ubs_std_defect_memleak_02();
	ret += ubs_std_defect_bof_02();
	ret += ubs_std_defect_uninit_02();
	ret += ubs_std_defect_doublefree_02(flag);
	ret += ubs_std_defect_uaf_02();
	ret += ubs_std_defect_memleak_03();
	ret += ubs_std_defect_bof_03();
	ret += ubs_std_defect_uninit_03();
	ret += ubs_std_defect_doublefree_03();
	ret += ubs_std_defect_uaf_03();
	ret += ubs_std_defect_memleak_04();
	ret += ubs_std_defect_bof_04();
	ret += ubs_std_defect_uninit_04();
	ret += ubs_std_defect_doublefree_04(flag);
	ret += ubs_std_defect_uaf_04();
	ret += ubs_std_defect_memleak_05(flag);
	ret += ubs_std_defect_bof_05();
	ret += ubs_std_defect_uninit_05(flag);
	ret += ubs_std_defect_doublefree_05(flag);
	ret += ubs_std_defect_uaf_05();

	return ret;
}

module_init(sentry_defects_init);
module_exit(sentry_defects_exit);
