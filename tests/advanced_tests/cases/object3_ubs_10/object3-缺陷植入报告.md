# 内存缺陷植入报告

## 概述

本项目在 `source/rpmbuild/BUILD/ubs-engine-1.0.0/src/sdk` 路径下的源文件中植入了5类内存缺陷，每类2个，共计10个。所有缺陷均植入在原始代码逻辑中，使用原始数据类型，并以 `[DEFECT: ...]` 格式添加了详细注释。

**涉及文件：**
- `ubs_engine_mem.c` — 4个缺陷 (ML-2, BO-1, BO-2, UAF-1) + DF-2调用方
- `libubse_helper.c` — 4个缺陷 (ML-1, DF-1, DF-2被调用方, UI-2)
- `ubs_engine_topo.c` — 1个缺陷 (UAF-2)
- `ubs_error.c` — 1个缺陷 (UI-1)

> **重要说明**: 经核查，`ubse_api_buffer_free()` 的实现会先 `free()` 再置空 `buffer` 指针并清零 `length` 字段。因此涉及 UAF/DoubleFree 的缺陷均使用原始 `free()` 来避免自动清零导致缺陷无效。

---

## 一、Memory Leak（内存泄漏）— 2个

### ML-1：流敏感 + 跨函数泄漏

| 项目 | 内容 |
|------|------|
| **文件** | `libubse_helper.c` |
| **函数** | `ubse_mem_fd_desc_list_unpack` |
| **行号** | 332–349 |
| **缺陷类型** | 内存泄漏 |
| **敏感维度** | 流敏感（Flow-sensitive）、跨函数（Context-sensitive） |

**缺陷植入逻辑：**

原始代码中，循环解包每个 `fd_desc` 描述符。当 `mem_fd_desc_unpack` 失败时，无论当前是第几次迭代，都会释放已分配的 `*fd_descs` 数组并置NULL后返回错误码。

植入缺陷后：当解包失败发生在非首次迭代（`i > 0`）时，跳过 `free(*fd_descs)` 和 `*fd_descs = NULL` 操作，直接返回错误码。此时 `*fd_descs` 指向的 `calloc` 分配的内存块（含 `*fd_desc_cnt` 个 `ubs_mem_fd_desc_t` 元素）将永远无法被释放，造成内存泄漏。

- **流敏感**：泄漏仅当 `i > 0` 时触发（第2个及之后的描述符解包失败），`i == 0` 时正常释放。控制流中迭代索引 `i` 的不同取值决定了是否泄漏。
- **跨函数**：触发泄漏的错误码来自 `mem_fd_desc_unpack` 内部校验（如 `memid_cnt` 合法性检查、剩余长度校验等），调用者无法预知哪个迭代会失败。

```c
if (ret != UBS_SUCCESS) {
    if (i == 0) {          // 仅首元素失败时释放
        free(*fd_descs);
        *fd_descs = NULL;
    }
    return ret;            // i > 0 时跳过释放 → 泄漏
}
```

---

### ML-2：域敏感 + 复杂运算泄漏

| 项目 | 内容 |
|------|------|
| **文件** | `ubs_engine_mem.c` |
| **函数** | `ubs_mem_fd_create` |
| **行号** | 106–120 |
| **缺陷类型** | 内存泄漏 |
| **敏感维度** | 域敏感（Field-sensitive）、复杂运算 |

**缺陷植入逻辑：**

原始代码中，IPC调用 `ubse_invoke_call` 返回后，无论成功或失败都会先释放 `request_buffer`，然后在 `ret != UBS_SUCCESS` 的错误分支中释放 `response_buffer` 后返回。

植入缺陷后：当IPC调用失败时，新增一个条件判断——若 `owner` 指针非空且 `owner->uid ^ owner->gid ^ owner->pid == 0`（即三个成员字段XOR结果为0，意味着uid/gid/pid同时为0的异常情况），函数跳过 `ubse_api_buffer_free(&response_buffer)` 直接返回 `UBS_ERR_OPERATION_FAILED`，造成 `response_buffer` 内存泄漏。

- **域敏感**：泄漏触发取决于 `owner` 指向结构体的三个成员字段（`uid`、`gid`、`pid`）的值。
- **复杂运算**：通过三个字段的XOR运算（`owner->uid ^ owner->gid ^ owner->pid`）组合判断是否进入泄漏分支。

```c
if (owner != NULL && (owner->uid ^ owner->gid ^ owner->pid) == 0) {
    return UBS_ERR_OPERATION_FAILED;  // 跳过ubse_api_buffer_free → 泄漏
}
```

---

## 二、Double Free（双重释放）— 2个

### DF-1：流敏感 + 复杂分支双重释放

| 项目 | 内容 |
|------|------|
| **文件** | `libubse_helper.c` |
| **函数** | `ubse_node_list_unpack` |
| **行号** | 523–541 |
| **缺陷类型** | 双重释放 |
| **敏感维度** | 流敏感（Flow-sensitive）、复杂分支 |

**缺陷植入逻辑：**

原始代码中，循环解包每个 `node`。当 `ubse_node_unpack_inner` 失败时，释放 `*node_list` 并置NULL后返回。

植入缺陷后：当解包失败发生在首次迭代（`i == 0`）且错误码为 `UBS_ERR_BUFFER_TOO_SMALL` 时，先执行一次 `free(*node_list)`，再执行第二次 `free(*node_list)`，导致双重释放。

- **流敏感**：双重释放仅在 `i == 0 && ret == UBS_ERR_BUFFER_TOO_SMALL` 的特定分支组合下触发。其他迭代（`i > 0`）或错误码不会触发双重释放。
- **复杂分支**：需要同时满足迭代索引和错误码两个条件的AND组合。
- **跨函数**：错误码来自 `ubse_node_unpack_inner` 内部的 `memcpy_s` 校验，调用者无法直接感知。

```c
if (ret == UBS_ERR_BUFFER_TOO_SMALL && i == 0) {
    free(*node_list);      // 第一次释放
}
free(*node_list);          // 第二次释放 → 双重释放
```

---

### DF-2：跨函数双重释放

| 项目 | 内容 |
|------|------|
| **文件** | `libubse_helper.c` + `ubs_engine_mem.c` |
| **函数** | `ubse_mem_numa_desc_list_unpack`（被调用方）+ `ubs_mem_numa_list`（调用方） |
| **行号** | libubse_helper.c:401–417, ubs_engine_mem.c:476–485 |
| **缺陷类型** | 双重释放 |
| **敏感维度** | 跨函数（Context-sensitive） |

**缺陷植入逻辑：**

此缺陷分两部分植入，跨两个函数协作触发：

**被调用方**（`ubse_mem_numa_desc_list_unpack`）：原始代码中，解包失败时释放 `*numa_descs` 并置NULL。植入后：当解包失败发生在最后一个元素（`i == *numa_desc_cnt - 1`）时，释放 `*numa_descs` 但**故意不置NULL**，使调用方能检测到非空指针。

```c
free(*numa_descs);
if (i != *numa_desc_cnt - 1) {
    *numa_descs = NULL;   // 最后一个元素失败时不置NULL
}
```

**调用方**（`ubs_mem_numa_list`）：原始代码中，解包调用后直接 `ubse_api_buffer_free` 并返回。植入后：新增错误检查——若解包失败且 `*numa_descs != NULL`，再次 `free(*numa_descs)`。

```c
if (ret != UBS_SUCCESS && *numa_descs != NULL) {
    free(*numa_descs);    // 第二次释放 → 双重释放
}
```

- **跨函数**：第一次 `free` 发生在 `ubse_mem_numa_desc_list_unpack`（被调用方），第二次 `free` 发生在 `ubs_mem_numa_list`（调用方），跨越两个函数形成双重释放。

---

## 三、Buffer Overflow（缓冲区溢出）— 2个

### BO-1：跨函数缓冲区溢出

| 项目 | 内容 |
|------|------|
| **文件** | `ubs_engine_mem.c` |
| **函数** | `ubs_mem_numa_create` |
| **行号** | 317–327 |
| **缺陷类型** | 缓冲区溢出（堆溢出） |
| **敏感维度** | 跨函数（Context-sensitive） |

**缺陷植入逻辑：**

原始代码中，`total_len` 计算包含 name（48字节）+ size（8字节）+ distance（4字节）= 60字节，并在 `malloc` 后由 `ubse_mem_numa_create_req_pack` 写入恰好60字节数据。

植入缺陷后：`total_len` 计算中遗漏 `sizeof(uint32_t)`（distance字段），仅分配56字节（48+8）。而 `ubse_mem_numa_create_req_pack` 仍按原逻辑写满60字节——先写48字节name，再写8字节size，最后**越过缓冲区边界4字节**写入 `distance` 字段（`*(uint32_t *)ptr = htonl((uint32_t)distance)`）。

- **跨函数**：缓冲区分配在 `ubs_mem_numa_create`（调用方），越界写入发生在 `ubse_mem_numa_create_req_pack`（被调用方）。调用方与被调用方对缓冲区大小的认知不一致导致溢出。

```c
// 分配端：少算sizeof(uint32_t)
const size_t total_len = UBS_MEM_MAX_NAME_LENGTH + sizeof(uint64_t);  // 56字节

// 写入端：按完整格式写60字节 → 越界4字节
*(uint32_t *)ptr = htonl((uint32_t)distance);  // 写在第57-60字节，超出56字节边界
```

---

### BO-2：域敏感 + 复杂运算缓冲区溢出

| 项目 | 内容 |
|------|------|
| **文件** | `ubs_engine_mem.c` |
| **函数** | `ubs_mem_fd_create_with_lender` |
| **行号** | 138–154 |
| **缺陷类型** | 缓冲区溢出（堆溢出） |
| **敏感维度** | 域敏感（Field-sensitive）、复杂运算、跨函数 |

**缺陷植入逻辑：**

原始代码中，`total_len` 包含 `sizeof(ubs_mem_lender_t)`（编译器决定的结构体大小，可能含padding，通常24或32字节），由 `ubse_mem_fd_create_with_lender_req_pack` 调用 `ubse_mem_lender_pack` 写入 lender 数据。

植入缺陷后：使用条件表达式计算 lender 部分的大小——当 `lender->lender_size > UBS_MEM_MAX_SIZE` 时使用 `sizeof(ubs_mem_lender_t)`（完整大小），否则使用 `sizeof(uint32_t)+sizeof(uint32_t)+sizeof(uint64_t)`（仅16字节，遗漏了 `lender_size` 字段的8字节）。

- **域敏感**：条件表达式的分支取决于 `lender->lender_size` 成员字段的值。由于合法输入始终满足 `lender_size <= UBS_MEM_MAX_SIZE`，总是进入 truncated 分支。
- **复杂运算**：使用条件表达式（三元运算符）+ 字段值与宏的比较来决定分配大小。
- **跨函数**：`ubse_mem_lender_pack` 始终写入完整的24字节lender数据（slot_id 4B + socket_id 4B + numa_id 8B + lender_size 8B），最后8字节越过调用者分配的16字节边界。

```c
(lender->lender_size > UBS_MEM_MAX_SIZE ?
    sizeof(ubs_mem_lender_t) :                           // 完整大小（走不到）
    (sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint64_t)))  // 16字节，缺lender_size
```

---

## 四、Uninitialized Use（未初始化使用）— 2个

### UI-1：流敏感未初始化使用

| 项目 | 内容 |
|------|------|
| **文件** | `ubs_error.c` |
| **函数** | `ubs_error_name` |
| **行号** | 38–54 |
| **缺陷类型** | 未初始化使用 |
| **敏感维度** | 流敏感（Flow-sensitive） |

**缺陷植入逻辑：**

原始代码中，局部指针 `info` 在声明时即通过 `ubs_get_error_info(error)` 初始化，随后判断是否为NULL。

植入缺陷后：`info` 声明时不再初始化，仅在 `error >= 0` 的分支中通过 `ubs_get_error_info(error)` 赋值。当 `error < 0` 时，`info` 保持未初始化状态（栈上的垃圾值）即参与 `info != NULL` 判断。

- **流敏感**：`info` 的初始化取决于 `error` 值的符号。`error >= 0` 时正常初始化，`error < 0` 时读取未初始化栈数据。控制流的分支决定了是否触发未定义行为。
- 虽然后续 `NULL` 比较在多数情况下不崩溃，但属于明确的未定义行为，可能在某些编译器/优化级别下产生不可预测的结果。

```c
const ubs_error_info_t *info;     // 未初始化
if (error >= 0) {
    info = ubs_get_error_info(error);  // 仅在此分支初始化
}
if (info != NULL) {               // error < 0 时读取未初始化值
    return info->name;
}
```

---

### UI-2：域敏感未初始化使用

| 项目 | 内容 |
|------|------|
| **文件** | `libubse_helper.c` |
| **函数** | `ubse_node_unpack_inner` |
| **行号** | 423–451 |
| **缺陷类型** | 未初始化使用 |
| **敏感维度** | 域敏感（Field-sensitive）、跨函数 |

**缺陷植入逻辑：**

原始代码中，循环 `UBS_TOPO_SOCKET_NUM`（值为2）次，初始化 `node->socket_id[0]` 和 `node->socket_id[1]`。

植入缺陷后：先读取 `node->slot_id` 字段，根据其值决定 `socket_count`——若 `slot_id == 0` 则只初始化 `socket_id[0]`（1个），否则正常初始化2个。`socket_id[1]` 在 `slot_id == 0` 时保持未初始化状态。为了保证后续 `host_name` 读取位置正确，`ptr` 通过补偿计算跳过来自网络数据流中未读取的 `socket_id[1]` 字节。

- **域敏感**：`socket_id[1]` 的初始化与否取决于 `node->slot_id` 字段的值。`slot_id == 0` 时 `socket_id[1]` 未初始化，`slot_id != 0` 时正常初始化。
- **跨函数**：`slot_id` 的值来自网络数据流反序列化（`ntohl` 转换），非本地可预测。调用者（如 `ubs_topo_node_local_get`）会将 `node` 结构体返回给上层使用。

```c
int socket_count = (node->slot_id == 0) ? 1 : UBS_TOPO_SOCKET_NUM;
for (int i = 0; i < socket_count; i++) {
    node->socket_id[i] = ntohl(*(const uint32_t *)ptr);  // socket_id[1]可能未初始化
    ptr += sizeof(uint32_t);
}
ptr += (UBS_TOPO_SOCKET_NUM - socket_count) * sizeof(uint32_t);  // 指针补偿
```

---

## 五、Use After Free（使用后释放）— 2个

### UAF-1：流敏感 + 复杂分支使用后释放

| 项目 | 内容 |
|------|------|
| **文件** | `ubs_engine_mem.c` |
| **函数** | `ubs_mem_fd_list` |
| **行号** | 249–268 |
| **缺陷类型** | 使用后释放（UAF） |
| **敏感维度** | 流敏感（Flow-sensitive）、复杂分支 |

**缺陷植入逻辑：**

原始代码中，IPC调用失败时调用 `ubse_api_buffer_free(&response_buffer)` 安全释放并清零，然后返回错误码。

植入缺陷后：IPC调用失败时，使用原始 `free(response_buffer.buffer)` 释放堆内存（不置空指针/不置零长度），然后通过悬空指针 `response_buffer.buffer` **读取已释放堆内存的首字节**（`response_buffer.buffer[0]`）做条件判断——若为 `0xFF` 则走特殊返回路径。

- **流敏感**：UAF仅在 `ret != IPC_SUCCESS` 的出错分支中触发。正常路径中 `response_buffer` 通过 `ubse_api_buffer_free` 正常释放。
- **复杂分支**：嵌套 `if` 条件中先检查悬空指针非空，再读取已释放内存内容，最后决定返回码——涉及三层条件判断。
- **关键设计**：使用原始 `free()` 而非 `ubse_api_buffer_free()`，因为后者会置空 `buffer` 指针和清零 `length`，导致无法访问已释放的堆内存。`response_buffer.buffer[0]` 直接解引用已释放堆内存。

```c
free(response_buffer.buffer);              // 原始free: 不置空指针
if (response_buffer.buffer != NULL && response_buffer.length > 0 &&
    response_buffer.buffer[0] == 0xFF) {   // UAF: 读取已释放堆内存首字节
    ...
}
```

---

### UAF-2：跨函数 + 域敏感使用后释放

| 项目 | 内容 |
|------|------|
| **文件** | `ubs_engine_topo.c` + `libubse_helper.c` |
| **函数** | `ubs_topo_node_local_get`（释放方）+ `ubse_node_unpack_inner`（读取方） |
| **行号** | ubs_engine_topo.c:65–75, libubse_helper.c:423–451 |
| **缺陷类型** | 使用后释放（UAF） |
| **敏感维度** | 跨函数（Context-sensitive）、域敏感（Field-sensitive） |

**缺陷植入逻辑：**

原始代码中，IPC成功后先调用 `ubse_node_unpack` 解包，再 `ubse_api_buffer_free(&response_buffer)` 释放。

**释放方**（`ubs_topo_node_local_get`）：植入后，使用原始 `free(response_buffer.buffer)` **提前释放**堆内存，但不置空指针/不置零长度。然后将悬空指针 `response_buffer.buffer` 和原始 `response_buffer.length`（仍保留有效值，可绕过 `ubse_node_unpack` 中的长度校验）传递给 `ubse_node_unpack`。

**读取方**（`ubse_node_unpack_inner`）：通过 `ubse_node_unpack` 间接调用。函数内从已释放的堆内存中读取 `slot_id`（`node->slot_id = ntohl(*(const uint32_t *)ptr)`），根据该字段值决定 `socket_count`（UI-2 缺陷配合）。所有字段（`socket_id[0]`、可能的 `socket_id[1]`、`host_name`）均从已释放堆内存中读取。

- **跨函数**：`free()` 在 `ubs_topo_node_local_get`，堆内存读取在 `ubse_node_unpack` → `ubse_node_unpack_inner` 调用链。
- **域敏感**：`socket_count` 取值取决于从已释放内存中读取的 `slot_id` 字段。释放后内存可能被堆分配器复用或修改，`slot_id` 值不可预测，导致 `socket_count` 不确定。
- **关键设计**：使用原始 `free()` 而非 `ubse_api_buffer_free()`，否则 `buffer` 被置NULL后 `ubse_node_unpack` 的长度校验（`len < UBSE_NODE_SIZE`）会直接返回错误，不会进入实际的内存读取逻辑。

```c
// ubs_topo_node_local_get:
free(response_buffer.buffer);              // 原始free: 堆内存释放，指针/长度保留原值
ret = ubse_node_unpack(response_buffer.buffer, response_buffer.length, node);
// 由于response_buffer.length保留原值(>=UBSE_NODE_SIZE)，长度校验通过
// ubse_node_unpack_inner从已释放内存中读取数据

// ubse_node_unpack_inner:
node->slot_id = ntohl(*(const uint32_t *)ptr);  // UAF: 从已释放堆内存读取
int socket_count = (node->slot_id == 0) ? 1 : 2; // 域敏感: 分支取决于已释放内存中的值
```

---

## 缺陷分布汇总

| 编号 | 缺陷类型 | 文件 | 函数 | 敏感维度 |
|------|----------|------|------|----------|
| ML-1 | 内存泄漏 | libubse_helper.c | ubse_mem_fd_desc_list_unpack | 流敏感+跨函数 |
| ML-2 | 内存泄漏 | ubs_engine_mem.c | ubs_mem_fd_create | 域敏感+复杂运算 |
| DF-1 | 双重释放 | libubse_helper.c | ubse_node_list_unpack | 流敏感+复杂分支 |
| DF-2 | 双重释放 | libubse_helper.c + ubs_engine_mem.c | ubse_mem_numa_desc_list_unpack + ubs_mem_numa_list | 跨函数 |
| BO-1 | 缓冲区溢出 | ubs_engine_mem.c | ubs_mem_numa_create | 跨函数 |
| BO-2 | 缓冲区溢出 | ubs_engine_mem.c | ubs_mem_fd_create_with_lender | 域敏感+复杂运算+跨函数 |
| UI-1 | 未初始化使用 | ubs_error.c | ubs_error_name | 流敏感 |
| UI-2 | 未初始化使用 | libubse_helper.c | ubse_node_unpack_inner | 域敏感+跨函数 |
| UAF-1 | 使用后释放 | ubs_engine_mem.c | ubs_mem_fd_list | 流敏感+复杂分支 |
| UAF-2 | 使用后释放 | ubs_engine_topo.c + libubse_helper.c | ubs_topo_node_local_get + ubse_node_unpack_inner | 跨函数+域敏感 |

**按文件分布：**
- `ubs_engine_mem.c`：ML-2, BO-1, BO-2, UAF-1 + DF-2调用方部分
- `libubse_helper.c`：ML-1, DF-1, DF-2被调用方部分, UI-2
- `ubs_engine_topo.c`：UAF-2
- `ubs_error.c`：UI-1

**按敏感维度分布：**
- 流敏感：ML-1, DF-1, UI-1, UAF-1
- 域敏感：ML-2, BO-2, UI-2, UAF-2
- 跨函数：ML-1, DF-2, BO-1, BO-2, UI-2, UAF-2
- 复杂运算/分支：ML-2, DF-1, BO-2, UAF-1
