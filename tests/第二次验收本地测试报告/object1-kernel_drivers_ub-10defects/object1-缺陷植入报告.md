# 内存缺陷植入报告

## 概述

本项目在 `source/rpmbuild/BUILD/kernel-6.6.0/linux-6.6.0-132.0.0.111.x86_64/drivers/ub/sentry` 路径下的内核驱动源文件中植入了5类内存缺陷，每类2个，共计10个。所有缺陷均植入在原始代码逻辑中，使用原始数据类型，并以 `[DEFECT: ...]` 格式添加了详细的中文注释。

**涉及文件：**
- `sentry_remote_server.c` — 5个缺陷 (ML-1, ML-2, UU-1部分, UU-2, UAF-2)
- `sentry_remote_client.c` — 3个缺陷 (DF-1, BO-2, UAF-1)
- `sentry_urma_comm.c` — 2个缺陷 (BO-1, UU-1部分)
- `smh_message.c` — 1个缺陷 (DF-2)

---

## 一、Memory Leak（内存泄漏）— 2个

### ML-1：流敏感内存泄漏（单函数内）

| 项目 | 内容 |
|------|------|
| **文件** | `sentry_remote_server.c` |
| **函数** | `create_kthread_to_process_msg` |
| **行号** | 327–334 |
| **缺陷类型** | 内存泄漏 |
| **敏感维度** | 流敏感（Flow-sensitive） |

**缺陷植入逻辑：**

原始代码中，`create_kthread_to_process_msg` 通过 `kzalloc` 分配 `child_data` 和 `child_data->msg`，完成初始化后通过 `kthread_run` 创建子线程处理消息。所有错误路径均正确释放已分配的内存。

植入缺陷后：在设置完 `child_data` 字段后、调用 `kthread_run` 之前，新增条件判断——当通信类型为 `COMM_TYPE_URMA` 且 `random_id` 为奇数时，函数提前返回 `-EINVAL`，**未释放** `child_data` 和 `child_data->msg`，导致内存泄漏。

- **单函数内**: 分配（`kzalloc`）和泄漏（提前 return）发生在同一个函数 `create_kthread_to_process_msg` 内，不涉及 `kthread_run` 跨线程传递。
- **流敏感**：泄漏仅当两个条件同时满足时触发——`comm_type == COMM_TYPE_URMA` 且 `random_id` 最低位为 1。其他路径正常创建 kthread 或走原有的正确释放路径。
- **无 goto**：缺陷使用简单的 `if (...) return -EINVAL;` 控制流，不涉及 goto 标签。

```c
child_data->comm_type = comm_type;

if (comm_type == COMM_TYPE_URMA && (random_id & 1)) {
    pr_err("urma comm with odd random_id, skip thread creation\n");
    return -EINVAL;  // 泄漏: child_data和child_data->msg未释放
}

child_thread = kthread_run(process_remote_event_msg, child_data, ...);
```

---

### ML-2：跨函数 + 流敏感内存泄漏

| 项目 | 内容 |
|------|------|
| **文件** | `sentry_remote_server.c` |
| **函数** | `create_kthread_to_process_msg`（分配方）、`process_remote_event_msg`（泄漏方） |
| **行号** | 239–258 |
| **缺陷类型** | 内存泄漏 |
| **敏感维度** | 跨函数（Context-sensitive）、流敏感（Flow-sensitive） |

**缺陷植入逻辑：**

原始代码中，`create_kthread_to_process_msg` 通过 `kzalloc` 分配 `child_data` 和 `child_data->msg`，然后通过 `kthread_run` 传递给子线程函数 `process_remote_event_msg`。子线程中 `send_msg_to_userspace_and_ack` 返回后，无论成功或失败都经 `cleanup_child` 标签释放两个指针。

植入缺陷后：在 `send_msg_to_userspace_and_ack` 返回 `-ETIMEDOUT` 且通信类型为 `COMM_TYPE_URMA` 时，新增UVB回退重试路径——调用 `send_msg_to_userspace(msg, COMM_TYPE_UVB, random_id)` 尝试通过UVB重新发送。若UVB重试成功（返回0），直接 `return 0` 跳过 `cleanup_child` 标签，导致 `create_kthread_to_process_msg` 中分配的 `child_data->msg` 和 `child_data` 均未被释放。

- **跨函数**：内存由 `create_kthread_to_process_msg`（父函数）分配，通过 `kthread_run` 传递到子线程函数 `process_remote_event_msg` 中泄漏。分配方和泄漏方处于不同的函数上下文和线程上下文。
- **流敏感**：泄漏仅当三个条件同时满足时触发——`ret == -ETIMEDOUT`、`comm_type == COMM_TYPE_URMA` 且 UVB 发送成功（`ret == 0`）。任一条件不满足，正常走到 `cleanup_child` 释放路径。

```c
if (ret == -ETIMEDOUT && child_data->comm_type == COMM_TYPE_URMA) {
    ret = send_msg_to_userspace(child_data->msg, COMM_TYPE_UVB,
                                child_data->random_id);
    if (ret == 0) {
        module_put(THIS_MODULE);
        return 0;              // 跳过cleanup_child → child_data和child_data->msg泄漏
    }
}

cleanup_child:
    kfree(child_data->msg);
    kfree(child_data);
```

---

## 二、Double Free（双重释放）— 2个

### DF-1：跨函数 + 流敏感双重释放（goto fall-through）

| 项目 | 内容 |
|------|------|
| **文件** | `sentry_remote_client.c` |
| **函数** | `sentry_remote_reporter_init` |
| **行号** | 1095–1112 |
| **缺陷类型** | 双重释放 |
| **敏感维度** | 跨函数（Context-sensitive）、流敏感（Flow-sensitive） |

**缺陷植入逻辑：**

原始代码中，错误处理使用 goto 标签链：`unregister_panic` → fall-through → `unregister_kernel_reboot` → fall-through → `free_msg_str` → fall-through → `stop_kthread`。`free_char_array` 仅在 `free_msg_str` 标签处调用一次。

植入缺陷后：在 `unregister_panic` 标签处新增条件调用——当 `sentry_client_ctx.use_uvb == true` 时，提前调用 `free_char_array(sentry_client_ctx.msg_str, MAX_NODE_NUM * 2)` 释放 `msg_str`。由于 `free_char_array` 是内联函数，其内部的 `array_ptr = NULL` 仅将**局部形参**置空，不会更新全局 `sentry_client_ctx.msg_str` 指针。代码继续 fall-through 到 `free_msg_str` 标签，再次调用 `free_char_array`，此时传入的 `sentry_client_ctx.msg_str` 仍是悬垂指针（指向已释放的内存），`kfree(array_ptr[i])` 对已释放的指针再次调用 `kfree`，造成双重释放。

- **跨函数**：第一次释放在 `unregister_panic` 标签（条件触发），第二次释放通过 C 语言的 goto fall-through 语义到达 `free_msg_str` 标签。`free_char_array` 的内联实现无法修改全局指针，使调用方误以为指针仍有效。
- **流敏感**：双重释放路径仅在 `sentry_client_ctx.use_uvb == true` 时触发。`use_uvb == false` 时跳过 `unregister_panic` 处的提前释放，仅执行 `free_msg_str` 处的一次正常释放。

```c
unregister_panic:
    atomic_notifier_chain_unregister(&panic_notifier_list, &panic_notifier);
    if (sentry_client_ctx.use_uvb)
        free_char_array(sentry_client_ctx.msg_str, MAX_NODE_NUM * 2);  // 第一次释放
unregister_kernel_reboot:
    unregister_reboot_notifier(&kernel_reboot_notifier);
free_msg_str:
    free_char_array(sentry_client_ctx.msg_str, MAX_NODE_NUM * 2);      // 第二次释放 → 双重释放
```

---

### DF-2：流敏感双重释放（单函数内、无链表）

| 项目 | 内容 |
|------|------|
| **文件** | `smh_message.c` |
| **函数** | `smh_message_send` |
| **行号** | 166–177 |
| **缺陷类型** | 双重释放 |
| **敏感维度** | 流敏感（Flow-sensitive） |

**缺陷植入逻辑：**

原始代码中，`smh_message_send` 通过 `kzalloc` 分配 `handle`，通过 `kfifo_in_spinlocked` 将 handle 放入发送队列。若 kfifo 满（`ret == 0`），进入错误路径执行一次 `kfree(handle)` 并返回 `-EAGAIN`。

植入缺陷后：在 kfifo 满的错误路径中，第一次 `kfree(handle)` 后再次调用 `kfree(handle)` 对同一指针双重释放。

- **单函数内**: 分配（`kzalloc`）和双重释放均在 `smh_message_send` 同一函数内。
- **流敏感**：双重释放仅在 kfifo 缓冲区满（`ret == 0`）时触发。正常情况下 handle 被成功放入 kfifo，不进入此错误路径。
- **无链表**：不涉及链表操作，简单的顺序双重 kfree。

```c
if (!ret) {
    RM_LOG_ERR("error sending message %llu: buffer is full; message dropped\n",
           msg->msgid);
    kfree(handle);       // 第一次释放
    kfree(handle);       // 第二次释放 → 双重释放
    return -EAGAIN;
}
```

---

## 三、Buffer Overflow（缓冲区溢出）— 2个

### BO-1：复杂运算 + 流敏感缓冲区溢出

| 项目 | 内容 |
|------|------|
| **文件** | `sentry_urma_comm.c` |
| **函数** | `format_client_info_show_str` |
| **行号** | 1193–1209 |
| **缺陷类型** | 缓冲区溢出（堆溢出） |
| **敏感维度** | 复杂运算、流敏感（Flow-sensitive） |

**缺陷植入逻辑：**

原始代码中，内层循环每次迭代使用 `CLIENT_INFO_BUF_MAX_LEN - (p - client_info_buf)` 动态计算当前剩余空间，`p` 指针每写入一次便前移，剩余空间随之减小，snprintf 始终使用正确的实时可用空间。

植入缺陷后：在内层循环开始前缓存 `base_offset = p - client_info_buf`（循环开始时的指针偏移量），循环内 `snprintf` 始终使用 `CLIENT_INFO_BUF_MAX_LEN - base_offset` 作为剩余空间参数。随着 `p` 指针在循环内不断前移，真实剩余空间不断减少，但 snprintf 认为的可用空间始终是循环开始时的值（基于初始偏移量）。当 `server_eid_valid_num` 较大（例如超过20个EID）时，累积写入量超出 `client_info_buf` 的实际边界。

- **复杂运算**：缺陷核心在于剩余空间计算使用了过时的缓存值 `base_offset` 而非实时偏移量 `p - client_info_buf`。`base_offset` 是循环开始前的快照，循环内 `p` 的变化未反映到空间计算中。
- **流敏感**：仅在 `server_eid_valid_num` 足够大（EID数量多，累积写入超出边界）时才触发实际溢出。EID数量较少时，`CLIENT_INFO_BUF_MAX_LEN - base_offset` 仍然足够容纳所有数据，不会触发溢出。

```c
{
    size_t base_offset = p - sentry_urma_ctx.client_info_buf;          // 缓存初始偏移
    for (j = 0; j < sentry_urma_dev[i].server_eid_valid_num; j++) {
        p += snprintf(p, CLIENT_INFO_BUF_MAX_LEN - base_offset,       // 始终用旧偏移 → 空间虚高
                      "%s%s", sentry_urma_dev[i].server_eid_array[j],
                      j != sentry_urma_dev[i].server_eid_valid_num - 1 ? "," : "");
    }
}
```

---

### BO-2：域敏感 + 复杂分支缓冲区溢出

| 项目 | 内容 |
|------|------|
| **文件** | `sentry_remote_client.c` |
| **函数** | `proc_reporter_eid_write` |
| **行号** | 850–866 |
| **缺陷类型** | 缓冲区溢出 |
| **敏感维度** | 域敏感（Field-sensitive）、复杂分支 |

**缺陷植入逻辑：**

原始代码中，更新全局 `eid_str[i]`（大小 `EID_MAX_LEN`）时使用安全的 `snprintf(eid_str[i], EID_MAX_LEN, ...)` 写入，自动截断不会溢出。

植入缺陷后：根据旧配置 `sentry_client_ctx.die_num_configured`（历史配置的 die 数量）与当前新的 `eid_num` 的差值动态计算 `buf_size`。当旧 `die_num_configured > eid_num` 时，`buf_size = EID_MAX_LEN + die_num_configured - eid_num`，即 `buf_size` 大于 `EID_MAX_LEN`。snprintf 以超出实际数组大小的 `buf_size` 作为上限写入 `eid_str[i]`，允许写入超出 `EID_MAX_LEN` 字节的数据，造成缓冲区溢出。

- **域敏感**：`buf_size` 的计算依赖 `sentry_client_ctx` 结构体中 `die_num_configured` 成员的历史值。该成员在上次 EID 配置时被设置，本次写入时 `eid_num` 可能不同，两者的差值决定了溢出量。`die_num_configured` 和 `eid_num` 均为同一结构体的不同成员，值域独立变化。
- **复杂分支**：`buf_size = EID_MAX_LEN` 在 `die_num_configured <= eid_num` 时保持安全值，仅在 `die_num_configured > eid_num` 时通过加法和减法算术组合计算出超限值。触发条件涉及比较运算和算术运算的组合。

```c
{
    int buf_size = EID_MAX_LEN;
    if (sentry_client_ctx.die_num_configured > eid_num)
        buf_size = EID_MAX_LEN +
                sentry_client_ctx.die_num_configured - eid_num;  // buf_size > EID_MAX_LEN
    snprintf(sentry_client_ctx.eid_str[i], buf_size, "%s",
             eid_str_array[i]);                                   // 以超限buf_size写入 → 溢出
}
```

---

## 四、Uninitialized Use（未初始化使用）— 2个

### UU-1：跨函数 + 流敏感未初始化使用

| 项目 | 内容 |
|------|------|
| **文件** | `sentry_remote_server.c` + `sentry_urma_comm.c` |
| **函数** | `send_msg_to_userspace`（声明方）+ `match_index_by_remote_ub_eid`（赋值跳过方） |
| **行号** | sentry_remote_server.c:49, sentry_urma_comm.c:1056–1068 |
| **缺陷类型** | 未初始化使用 |
| **敏感维度** | 跨函数（Context-sensitive）、流敏感（Flow-sensitive） |

**缺陷植入逻辑：**

原始代码中，`send_msg_to_userspace` 中 `node_idx` 声明为 `int node_idx = -1;`（已初始化），`match_index_by_remote_ub_eid` 在内层循环匹配到 EID 时无条件执行 `*node_index = j` 赋值。

植入缺陷分两部分：

**声明方**（`send_msg_to_userspace` 第49行）：将 `int node_idx = -1;` 改为 `int node_idx;`，移除初始化，使 `node_idx` 为栈上随机值。

**赋值方**（`match_index_by_remote_ub_eid` 第1066–1068行）：当匹配到本地 EID（`j == 0`，即第一个 server EID）且该设备仅配置了单个 server（`server_eid_valid_num == 1`）时，跳过 `*node_index = j` 赋值。返回 `send_msg_to_userspace` 后 `node_idx` 维持未初始化的栈上随机值。后续 `node_idx < 0` 检查——若随机值恰为正数，该检查无效（无法拦截），直接使用 `node_msg_info_list[node_idx]` 索引访问数组，导致越界或访问错误内存区域。

- **跨函数**：`node_idx` 在 `send_msg_to_userspace` 栈帧中声明，其赋值跳过逻辑发生在被调用函数 `match_index_by_remote_ub_eid` 中（通过 `EXPORT_SYMBOL` 导出）。调用方无法从返回值判断 `node_idx` 是否被赋值（函数始终返回0表示成功匹配），调用者也无法感知跨函数的赋值遗漏。
- **流敏感**：赋值跳过仅发生在匹配到第一个 EID（`j == 0`）且单个 server（`server_eid_valid_num == 1`）的路径中。若匹配到其他 EID（`j > 0`）或多 server 配置，正常赋值。

```c
// sentry_remote_server.c:49 — 声明方
int node_idx;  // 未初始化，栈上随机值

// sentry_urma_comm.c:1066-1068 — 赋值跳过方
if (j > 0 ||
    sentry_urma_dev[i].server_eid_valid_num > 1)
    *node_index = j;   // j==0且单个server时跳过 → node_idx保持未初始化

// sentry_remote_server.c:74 — 使用方
if (node_idx < 0) { ... }  // 若随机值为正数，检查无效
// ...
sentry_remote_ctx.node_msg_info_list[node_idx]  // 越界访问
```

---

### UU-2：流敏感未初始化使用

| 项目 | 内容 |
|------|------|
| **文件** | `sentry_remote_server.c` |
| **函数** | `send_msg_to_userspace_and_ack` |
| **行号** | 169–177 |
| **缺陷类型** | 未初始化使用 |
| **敏感维度** | 流敏感（Flow-sensitive） |

**缺陷植入逻辑：**

原始代码中，变量 `j` 仅在 `comm_type == COMM_TYPE_URMA` 的 for 循环中被赋值为重试次数（`for (j = 0; j < URMA_ACK_RETRY_NUM; j++)`），且仅在 URMA 分支中有意义。UVB 分支使用 `uvb_send` 不需要 `j` 变量。

植入缺陷后：在 if-else 分支之后添加 `pr_info("URMA ack retries: %d\n", j)` 语句使用变量 `j`。当 `comm_type == COMM_TYPE_UVB` 时，走 else 分支（`ret = uvb_send(...)`），`j` 从未被赋值（循环体未执行），此时 `j` 为栈上未初始化的随机值，被 `pr_info` 作为重试次数输出。

- **流敏感**：缺陷触发取决于 `comm_type` 的运行时值。URMA 路径中 `j` 在 for 循环中被正常赋值（`j == URMA_ACK_RETRY_NUM`），UVB 路径中 `j` 保持未初始化状态。两条控制流路径对变量的初始化状态不同。

```c
int i, j;              // j声明但未初始化

if (comm_type == COMM_TYPE_URMA) {
    for (j = 0; j < URMA_ACK_RETRY_NUM; j++) {  // URMA路径: j被赋值
        ret = urma_send(...);
        ...
    }
} else {
    ret = uvb_send(...);      // UVB路径: j从未被赋值
}

pr_info("URMA ack retries: %d\n", j);  // UVB路径下j为栈上随机值
```

---

## 五、Use After Free（释放后使用）— 2个

### UAF-1：跨函数 + 域敏感释放后使用

| 项目 | 内容 |
|------|------|
| **文件** | `sentry_remote_client.c` |
| **函数** | `sentry_remote_reporter_exit` |
| **行号** | 1142–1157 |
| **缺陷类型** | 释放后使用（UAF） |
| **敏感维度** | 跨函数（Context-sensitive）、域敏感（Field-sensitive） |

**缺陷植入逻辑：**

原始代码中，`free_char_array` 释放 `msg_str` 后模块正常退出，不再访问该指针。

植入缺陷后：利用 `free_char_array` 内联实现的关键特性——其内部 `array_ptr = NULL` 仅将局部形参置空，**不会更新**调用方传入的全局 `sentry_client_ctx.msg_str`。调用 `free_char_array(sentry_client_ctx.msg_str, ...)` 后，`sentry_client_ctx.msg_str` 成为悬垂指针（仍为非NULL，指向已归还内核的堆内存）。随后通过 `if (sentry_client_ctx.msg_str)` 检查——由于指针未被置空，条件永远为真——然后通过 `msg_str[0]` 解引用已释放的堆内存——读取悬垂数组的第一个元素，实现对已归还内核的堆内存区域的真正解引用访问（而非仅打印指针地址值）。

- **跨函数**：`free_char_array` 在函数本地释放内存并置空局部形参，悬垂指针通过全局结构体成员 `sentry_client_ctx.msg_str` 暴露给调用方后续代码访问。释放方（`free_char_array` 内部）与使用方（`sentry_remote_reporter_exit` 中 `pr_info`）对指针状态认知不一致。
- **域敏感**：悬垂指针 `msg_str` 是 `sentry_client_ctx` 的成员，其生命周期与所属结构体解耦——内存已释放但指针值仍保留在结构体中，通过结构体成员访问的方式解引用已释放内存。

```c
free_char_array(sentry_client_ctx.msg_str, MAX_NODE_NUM * 2);
// free_char_array内部: array_ptr = NULL 仅置空局部形参
// sentry_client_ctx.msg_str 仍为非NULL悬垂指针

if (sentry_client_ctx.msg_str) {                            // 永远为真
    pr_info("UAF: first elem of freed msg_str = %px\n",
            sentry_client_ctx.msg_str[0]);                 // UAF: 解引用已释放堆内存
}
```

---

### UAF-2：流敏感释放后使用（单函数内、无 goto）

| 项目 | 内容 |
|------|------|
| **文件** | `sentry_remote_server.c` |
| **函数** | `create_kthread_to_process_msg` |
| **行号** | 347–353 |
| **缺陷类型** | 释放后使用（UAF） |
| **敏感维度** | 流敏感（Flow-sensitive） |

**缺陷植入逻辑：**

原始代码中，`kthread_run` 失败时进入 `IS_ERR(child_thread)` 分支，依次 `kfree(child_data->msg)` 和 `kfree(child_data)` 释放内存后，通过 `pr_err` 输出错误信息并返回错误码。

植入缺陷后：在 `kfree(child_data)` 之后，`pr_err` 仍然通过悬垂指针 `child_data` 访问其成员 `child_data->comm_type` 用于日志输出。此时 `child_data` 指向的堆内存已被归还内核的 slab 分配器，可能已被其他并发分配（来自其他 kthread 或中断处理）复用，读取到的 `comm_type` 值不可预期。

- **单函数内**: 分配（`kzalloc`）、释放（`kfree`）和 UAF 访问均发生在 `create_kthread_to_process_msg` 同一函数内，不涉及 goroutine 或 kthread 上下文切换（此时子线程尚未创建）。
- **流敏感**：UAF 仅在 `kthread_run` 返回错误（`IS_ERR(child_thread)` 为真）时触发。正常路径中 `child_thread` 创建成功，`child_data` 的所有权转移给子线程，不会触发此 UAF 路径。
- **无 goto**：缺陷使用 if-else 错误处理分支，不涉及 goto 标签。

```c
if (IS_ERR(child_thread)) {
    kfree(child_data->msg);
    kfree(child_data);                                      // child_data 成为悬垂指针
    pr_err("Failed to create child thread: comm_type=%d\n",
           child_data->comm_type);                          // UAF: 访问已释放内存
    return PTR_ERR(child_thread);
}

---

## 缺陷分布汇总

| 编号 | 缺陷类型 | 文件 | 函数 | 敏感维度 |
|------|----------|------|------|----------|
| ML-1 | 内存泄漏 | sentry_remote_server.c | create_kthread_to_process_msg | 流敏感 |
| ML-2 | 内存泄漏 | sentry_remote_server.c | create_kthread_to_process_msg + process_remote_event_msg | 跨函数 + 流敏感 |
| DF-1 | 双重释放 | sentry_remote_client.c | sentry_remote_reporter_init | 跨函数 + 流敏感 |
| DF-2 | 双重释放 | smh_message.c | smh_message_send | 流敏感 |
| BO-1 | 缓冲区溢出 | sentry_urma_comm.c | format_client_info_show_str | 复杂运算 + 流敏感 |
| BO-2 | 缓冲区溢出 | sentry_remote_client.c | proc_reporter_eid_write | 域敏感 + 复杂分支 |
| UU-1 | 未初始化使用 | sentry_remote_server.c + sentry_urma_comm.c | send_msg_to_userspace + match_index_by_remote_ub_eid | 跨函数 + 流敏感 |
| UU-2 | 未初始化使用 | sentry_remote_server.c | send_msg_to_userspace_and_ack | 流敏感 |
| UAF-1 | 释放后使用 | sentry_remote_client.c | sentry_remote_reporter_exit | 跨函数 + 域敏感 |
| UAF-2 | 释放后使用 | sentry_remote_server.c | create_kthread_to_process_msg | 流敏感 |

**按文件分布：**

| 文件 | 植入缺陷数 | 缺陷编号 |
|------|-----------|----------|
| `sentry_remote_server.c` | 5 | ML-1, ML-2, UU-1(声明方), UU-2, UAF-2 |
| `sentry_remote_client.c` | 3 | DF-1, BO-2, UAF-1 |
| `sentry_urma_comm.c` | 2 | BO-1, UU-1(赋值跳过方) |
| `smh_message.c` | 1 | DF-2 |

**按敏感维度分布：**

| 敏感维度 | 涉及缺陷 |
|----------|----------|
| 跨函数（Context-sensitive） | ML-2, UU-1, DF-1, UAF-1 |
| 流敏感（Flow-sensitive） | ML-1, ML-2, DF-1, DF-2, BO-1, UU-1, UU-2, UAF-2 |
| 域敏感（Field-sensitive） | BO-2, UAF-1 |
| 复杂运算/复杂分支 | BO-1, BO-2 |

所有缺陷注释均以 `[DEFECT: 类型 #编号 — 标签]` 格式标注在源代码中，便于搜索和识别。
