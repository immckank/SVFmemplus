# OpenHarmony 内存缺陷植入报告

## 概述

本次在 OpenHarmony 4.0 Release 的 WLAN client 模块中选取 5 个小批量 TU 植入缺陷，用于对 Cppcheck、Infer、Clang Static Analyzer（CSA）和 SVF 四类静态分析流程进行对比测试。共植入 5 类内存缺陷，每类 8 个，共计 40 个。

**涉及文件：**
- `drivers/peripheral/wlan/client/src/wifi_driver_client.c` — 9 个缺陷（ML-1, ML-2, DF-1, DF-2, BO-1, BO-2, UI-1, UAF-1, UAF-2）
- `drivers/peripheral/wlan/client/src/sbuf/sbuf_wpa_cmd_adapter.c` — 11 个缺陷（ML-3, ML-4, DF-3, DF-4, BO-3, BO-4, UI-2, UI-3, UI-4, UAF-3, UAF-4）
- `drivers/peripheral/wlan/client/src/sbuf/sbuf_event_adapter.c` — 5 个缺陷（ML-5, DF-5, BO-5, UI-5, UAF-5）
- `drivers/peripheral/wlan/client/src/sbuf/sbuf_cmd_adapter.c` — 5 个缺陷（ML-6, DF-6, BO-6, UI-6, UAF-6）
- `drivers/peripheral/wlan/client/src/sbuf/sbuf_common_adapter.c` — 10 个缺陷（ML-7, ML-8, DF-7, DF-8, BO-7, BO-8, UI-7, UI-8, UAF-7, UAF-8）

**compile_commands 片段：**
- `ohos_wlan_defects/compile_commands.fragment.json`
- `ohos_wlan_defects/compile_commands.fragment.inferok.json`（仅用于 Infer，去掉 Infer 自带 clang 不支持的 OpenHarmony 私有参数）

---

## 一、Memory Leak（内存泄漏）

### ML-1：回调注册特殊返回泄漏

| 项目 | 内容 |
|------|------|
| **文件** | `wifi_driver_client.c` |
| **函数** | `WifiRegisterEventCallback` |
| **行号** | 76–93 |
| **缺陷类型** | 内存泄漏 |
| **敏感维度** | 流敏感、域敏感 |

**缺陷植入逻辑：**

原始代码中，`callbackEvent` 分配后要么写入全局回调表，要么在失败路径释放。植入后，当 `eventType & 0x40000000U` 且 `ifName[0] != '\0'` 时，函数直接返回失败码，跳过 `free(callbackEvent)`。

```c
callbackEvent = (struct CallbackEvent *)malloc(sizeof(struct CallbackEvent));
...
if (((eventType & 0x40000000U) != 0) && ifName[0] != '\0') {
    return RET_CODE_FAILURE;  // 跳过 free(callbackEvent)
}
```

- **流敏感**：只有复制 `ifName` 成功后进入特殊分支才泄漏。
- **域敏感**：触发条件依赖 `eventType` 的高位标志和 `ifName` 内容。

---

### ML-2：Hid2d 回调注册早退泄漏

| 项目 | 内容 |
|------|------|
| **文件** | `wifi_driver_client.c` |
| **函数** | `WifiRegisterHid2dCallback` |
| **行号** | 166–173 |
| **缺陷类型** | 内存泄漏 |
| **敏感维度** | 流敏感、复杂分支 |

**缺陷植入逻辑：**

`event` 由 `OsalMemCalloc` 分配，原始失败路径会落到尾部 `OsalMemFree(event)`。植入后，在 `ifName[0]` 的特定位满足条件时提前返回，绕过尾部释放。

```c
event = (struct Hid2dEvent *)OsalMemCalloc(sizeof(struct Hid2dEvent));
...
if (((uint8_t)ifName[0] & 0x2U) != 0) {
    return RET_CODE_FAILURE;  // event 泄漏
}
```

- **流敏感**：泄漏只发生在分配成功后的特定早退路径。
- **复杂分支**：触发条件来自接口名首字节的位运算。

---

### ML-3：EAPOL 发送镜像缓冲早退泄漏

| 项目 | 内容 |
|------|------|
| **文件** | `sbuf_wpa_cmd_adapter.c` |
| **函数** | `WifiEapolPacketSend` |
| **行号** | 57–63 |
| **缺陷类型** | 内存泄漏 |
| **敏感维度** | 流敏感、复杂运算 |

**缺陷植入逻辑：**

为发送缓冲创建 `mirror` 副本后，新增一个长度高位条件。若 `length & 0x80000000U` 成立，函数直接返回，跳过 `free(mirror)`。

```c
mirror = (uint8_t *)malloc(length);
...
if ((length & 0x80000000U) != 0) {
    return RET_CODE_FAILURE;  // mirror 泄漏
}
```

- **流敏感**：只在 `mirror` 分配成功后触发。
- **复杂运算**：由长度字段的高位标志决定是否泄漏。

---

### ML-4：MAC 查询临时缓冲早退泄漏

| 项目 | 内容 |
|------|------|
| **文件** | `sbuf_wpa_cmd_adapter.c` |
| **函数** | `WifiCmdGetOwnMac` |
| **行号** | 419–425 |
| **缺陷类型** | 内存泄漏 |
| **敏感维度** | 流敏感、复杂运算 |

**缺陷植入逻辑：**

函数根据 `len` 分配 `macScratch`，正常路径会在函数尾部释放。植入后，当 `len` 高位满足条件时直接返回，导致 `macScratch` 泄漏。

```c
macScratch = (char *)malloc(len);
...
if ((len & 0x80000000U) != 0) {
    return RET_CODE_FAILURE;  // macScratch 泄漏
}
```

- **流敏感**：泄漏路径位于分配之后、统一释放之前。
- **复杂运算**：触发条件来自 `len` 的高位判断。

---

### ML-5：扫描结果 IE 复制早退泄漏

| 项目 | 内容 |
|------|------|
| **文件** | `sbuf_event_adapter.c` |
| **函数** | `FillScanResult` |
| **行号** | 143–154 |
| **缺陷类型** | 内存泄漏 |
| **敏感维度** | 流敏感、跨函数清理缺口 |

**缺陷植入逻辑：**

`dst->bssid` 和 `dst->ie` 已经分配并复制成功后，若 `src->ieLen & 0x80000000U` 成立，直接返回失败，绕过 `FreeScanResult(dst)`。

```c
if (memcpy_s(dst->ie, src->ieLen, src->ie, src->ieLen) != EOK) {
    break;
}
if ((src->ieLen & 0x80000000U) != 0) {
    return RET_CODE_FAILURE;  // 跳过 FreeScanResult(dst)
}
```

- **流敏感**：泄漏发生在 IE 复制成功之后、beacon IE 分配之前。
- **跨函数清理缺口**：早退未调用 `wifi_driver_client.c` 中的 `FreeScanResult()`。

---

### ML-6：接口名数组高位早退泄漏

| 项目 | 内容 |
|------|------|
| **文件** | `sbuf_cmd_adapter.c` |
| **函数** | `GetIfNames` |
| **行号** | 625–632 |
| **缺陷类型** | 内存泄漏 |
| **敏感维度** | 流敏感、域敏感 |

**缺陷植入逻辑：**

`*ifNames = calloc(...)` 成功后，若 `*num & 0x80000000U` 成立直接返回，调用方无法获得并释放该缓冲。

```c
*ifNames = (char *)calloc(allocNum, IFNAMSIZ);
...
if ((*num & 0x80000000U) != 0) {
    return RET_CODE_FAILURE;  // *ifNames 泄漏
}
```

- **流敏感**：泄漏只发生在 `calloc` 成功之后。
- **域敏感**：触发条件由 `*num` 高位标志控制。

---

### ML-7：服务释放状态分支泄漏

| 项目 | 内容 |
|------|------|
| **文件** | `sbuf_common_adapter.c` |
| **函数** | `ReleaseWifiService` |
| **行号** | 51–63 |
| **缺陷类型** | 内存泄漏 |
| **敏感维度** | 流敏感、字段敏感 |

**缺陷植入逻辑：**

`releaseTrace` 分配后，如果 `g_wifiService->dispatcher == NULL`，函数提前返回，跳过 `free(releaseTrace)`。

```c
releaseTrace = (char *)malloc(sizeof(uint32_t));
...
if (g_wifiService->dispatcher == NULL) {
    return;  // releaseTrace 泄漏
}
```

- **流敏感**：泄漏发生在分配成功后的特定早退路径。
- **字段敏感**：触发条件依赖 `dispatcher` 指针是否为空。

---

### ML-8：命令发送高位分支泄漏

| 项目 | 内容 |
|------|------|
| **文件** | `sbuf_common_adapter.c` |
| **函数** | `SendCmdSync` |
| **行号** | 92–98 |
| **缺陷类型** | 内存泄漏 |
| **敏感维度** | 流敏感、复杂分支 |

**缺陷植入逻辑：**

`cmdTrace` 分配后，若 `cmd & 0x80000000U` 成立直接返回，绕过尾部释放。

```c
cmdTrace = (char *)malloc(sizeof(uint32_t));
...
if ((cmd & 0x80000000U) != 0) {
    return RET_CODE_FAILURE;  // cmdTrace 泄漏
}
```

- **流敏感**：泄漏路径位于分配之后、统一释放之前。
- **复杂分支**：触发条件来自 `cmd` 的高位标志。

---

## 二、Double Free（双重释放）

### DF-1：回调名称复制失败双重释放

| 项目 | 内容 |
|------|------|
| **文件** | `wifi_driver_client.c` |
| **函数** | `WifiRegisterEventCallback` |
| **行号** | 85–89 |
| **缺陷类型** | 双重释放 |
| **敏感维度** | 流敏感 |

**缺陷植入逻辑：**

原始代码中，`strcpy_s` 失败时释放一次 `callbackEvent` 后返回。植入后在同一错误分支中连续释放两次同一指针。

```c
if (strcpy_s(callbackEvent->ifName, IFNAMSIZ, ifName) != RET_CODE_SUCCESS) {
    free(callbackEvent);
    free(callbackEvent);  // 第二次释放
    return RET_CODE_FAILURE;
}
```

- **流敏感**：只有复制失败路径触发。

---

### DF-2：注销回调条件双重释放

| 项目 | 内容 |
|------|------|
| **文件** | `wifi_driver_client.c` |
| **函数** | `WifiUnregisterEventCallback` |
| **行号** | 121–128 |
| **缺陷类型** | 双重释放 |
| **敏感维度** | 流敏感、域敏感 |

**缺陷植入逻辑：**

匹配到全局回调表项后，函数先释放 `g_callbackEventMap[i]`。植入后，当 `eventType & 0x08000000U` 成立时，再次释放同一表项。

```c
free(g_callbackEventMap[i]);
if ((eventType & 0x08000000U) != 0) {
    free(g_callbackEventMap[i]);  // 第二次释放
}
```

- **流敏感**：只有注销命中真实表项后触发。
- **域敏感**：第二次释放由 `eventType` 字段控制。

---

### DF-3：EAPOL 镜像缓冲条件双重释放

| 项目 | 内容 |
|------|------|
| **文件** | `sbuf_wpa_cmd_adapter.c` |
| **函数** | `WifiEapolPacketSend` |
| **行号** | 85–88 |
| **缺陷类型** | 双重释放 |
| **敏感维度** | 流敏感、复杂分支 |

**缺陷植入逻辑：**

`mirror` 在发送流程结束后统一释放。植入后，如果 `length & 0x40000000U` 成立，又执行第二次 `free(mirror)`。

```c
free(mirror);
if ((length & 0x40000000U) != 0) {
    free(mirror);  // 第二次释放
}
```

- **流敏感**：需要先走到函数尾部清理路径。
- **复杂分支**：由长度高位组合控制。

---

### DF-4：EAPOL 接收拷贝失败双重释放

| 项目 | 内容 |
|------|------|
| **文件** | `sbuf_wpa_cmd_adapter.c` |
| **函数** | `WifiEapolPacketReceive` |
| **行号** | 144–156 |
| **缺陷类型** | 双重释放 |
| **敏感维度** | 流敏感、跨函数 |

**缺陷植入逻辑：**

接收路径先从 `HdfSbufReadBuffer` 得到响应数据，再分配 `rxEapol->buf` 并 `memcpy_s`。植入后，当拷贝失败时，对 `rxEapol->buf` 连续释放两次。

```c
if (memcpy_s(rxEapol->buf, eapol.len, eapol.buf, eapol.len) != EOK) {
    free(rxEapol->buf);
    free(rxEapol->buf);  // 第二次释放
}
```

- **流敏感**：只有接收缓冲分配成功且拷贝失败时触发。
- **跨函数**：拷贝源数据来自 SBuf 反序列化接口。

---

### DF-5：扫描结果 IE 跨函数双重释放

| 项目 | 内容 |
|------|------|
| **文件** | `sbuf_event_adapter.c` |
| **函数** | `FillScanResult` |
| **行号** | 170–174 |
| **缺陷类型** | 双重释放 |
| **敏感维度** | 跨文件、跨函数、别名敏感 |

**缺陷植入逻辑：**

错误路径中先按条件 `OsalMemFree(dst->ie)`，随后调用 `FreeScanResult(dst)`，后者定义在 `wifi_driver_client.c`，再次释放 `dst->ie`。

```c
if (((src->beaconIeLen & 0x40000000U) != 0) && dst->ie != NULL) {
    OsalMemFree(dst->ie);
}
FreeScanResult(dst);  // 再次释放 dst->ie
```

- **跨文件**：`FreeScanResult()` 定义在 `wifi_driver_client.c`。
- **跨函数**：局部释放与统一清理函数叠加。
- **别名敏感**：`dst->ie` 经两次释放路径指向同一堆块。

---

### DF-6：接口名错误路径双重释放

| 项目 | 内容 |
|------|------|
| **文件** | `sbuf_cmd_adapter.c` |
| **函数** | `GetIfNames` |
| **行号** | 634–640 |
| **缺陷类型** | 双重释放 |
| **敏感维度** | 流敏感、复杂分支 |

**缺陷植入逻辑：**

`HdfSbufReadBuffer` 失败后先 `free(*ifNames)`，再在 `*num & 0x40000000U` 条件下第二次释放。

```c
free(*ifNames);
if ((*num & 0x40000000U) != 0) {
    free(*ifNames);  // 第二次释放
}
```

- **流敏感**：只有读缓冲失败路径触发。
- **复杂分支**：第二次释放由 `*num` 高位标志控制。

---

### DF-7：服务释放尾部双重释放

| 项目 | 内容 |
|------|------|
| **文件** | `sbuf_common_adapter.c` |
| **函数** | `ReleaseWifiService` |
| **行号** | 67–72 |
| **缺陷类型** | 双重释放 |
| **敏感维度** | 流敏感、尾部清理 |

**缺陷植入逻辑：**

`releaseTrace` 在回收服务对象前释放一次，随后在服务回收后再次释放。

```c
free(releaseTrace);
...
HdfIoServiceRecycle(g_wifiService);
free(releaseTrace);  // 第二次释放
```

- **流敏感**：需要先走到服务回收路径。
- **尾部清理**：两次释放位于同一函数尾部相邻位置。

---

### DF-8：命令发送条件双重释放

| 项目 | 内容 |
|------|------|
| **文件** | `sbuf_common_adapter.c` |
| **函数** | `SendCmdSync` |
| **行号** | 108–111 |
| **缺陷类型** | 双重释放 |
| **敏感维度** | 流敏感、复杂分支 |

**缺陷植入逻辑：**

`cmdTrace` 统一释放后，若 `cmd & 0x40000000U` 成立再次释放。

```c
free(cmdTrace);
if ((cmd & 0x40000000U) != 0) {
    free(cmdTrace);  // 第二次释放
}
```

- **流敏感**：需要先走到函数尾部清理路径。
- **复杂分支**：第二次释放由 `cmd` 高位标志控制。

---

## 三、Buffer Overflow（缓冲区溢出）

### BO-1：回调注册栈数组越界写

| 项目 | 内容 |
|------|------|
| **文件** | `wifi_driver_client.c` |
| **函数** | `WifiRegisterEventCallback` |
| **行号** | 81–83 |
| **缺陷类型** | 缓冲区溢出（栈越界） |
| **敏感维度** | 流敏感、复杂运算 |

**缺陷植入逻辑：**

`traceName` 长度为 `IFNAMSIZ`。植入后，当 `eventType` 特定位满足条件时，将索引设为 `IFNAMSIZ`，写入数组末尾后一字节。

```c
char traceName[IFNAMSIZ];
uint32_t traceIndex = ((eventType & 0x10000000U) != 0) ? IFNAMSIZ : 0;
traceName[traceIndex] = '\0';  // traceIndex == IFNAMSIZ 时越界
```

- **流敏感**：只在分配成功后的注册路径触发。
- **复杂运算**：越界索引由三元表达式和位标志共同决定。

---

### BO-2：Hid2d 回调接口名字段越界写

| 项目 | 内容 |
|------|------|
| **文件** | `wifi_driver_client.c` |
| **函数** | `WifiRegisterHid2dCallback` |
| **行号** | 175–180 |
| **缺陷类型** | 缓冲区溢出（结构体字段越界） |
| **敏感维度** | 域敏感、复杂运算 |

**缺陷植入逻辑：**

`event->ifName` 的合法最大索引为 `IFNAMSIZ`。植入后，`nameIndex` 可取 `IFNAMSIZ + 1`，并在 `ifName[0] & 0x4` 成立时写入该位置。

```c
uint32_t nameIndex = IFNAMSIZ + (((uint8_t)ifName[0]) & 0x1U);
if ((ifName[0] & 0x4) != 0) {
    event->ifName[nameIndex] = '\0';  // nameIndex 可能越界
}
```

- **域敏感**：越界写入目标是结构体成员 `ifName`。
- **复杂运算**：索引由接口名首字节低位计算得到。

---

### BO-3：EAPOL 本地 MAC 缓冲越界写

| 项目 | 内容 |
|------|------|
| **文件** | `sbuf_wpa_cmd_adapter.c` |
| **函数** | `WifiEapolPacketSend` |
| **行号** | 54–56 |
| **缺陷类型** | 缓冲区溢出（栈越界） |
| **敏感维度** | 流敏感、复杂运算 |

**缺陷植入逻辑：**

`localAddr` 长度为 `ETH_ADDR_LEN`。植入后，当 `length & 0x2U` 成立时，将索引设为 `ETH_ADDR_LEN`，写入数组越界位置。

```c
uint8_t localAddr[ETH_ADDR_LEN];
uint32_t addrIndex = ((length & 0x2U) != 0) ? ETH_ADDR_LEN : 0;
localAddr[addrIndex] = 0;  // addrIndex == ETH_ADDR_LEN 时越界
```

- **流敏感**：位于参数校验之后，仅有效输入触发。
- **复杂运算**：索引取值来自长度字段的位运算。

---

### BO-4：EAPOL 接收堆缓冲少分配

| 项目 | 内容 |
|------|------|
| **文件** | `sbuf_wpa_cmd_adapter.c` |
| **函数** | `WifiEapolPacketReceive` |
| **行号** | 139–153 |
| **缺陷类型** | 缓冲区溢出（堆越界） |
| **敏感维度** | 跨函数、复杂分支 |

**缺陷植入逻辑：**

原始代码按 `eapol.len` 分配目标缓冲并拷贝同样长度。植入后，当 `eapol.len & 0x20U` 成立时少分配 1 字节，但后续仍按 `eapol.len` 作为目标容量和拷贝长度传入 `memcpy_s`。

```c
uint32_t allocLen = eapol.len;
if (((eapol.len & 0x20U) != 0) && eapol.len > 1) {
    allocLen = eapol.len - 1;  // 少分配 1 字节
}
rxEapol->buf = malloc(allocLen);
memcpy_s(rxEapol->buf, eapol.len, eapol.buf, eapol.len);
```

- **跨函数**：`eapol.len` 来自 `HdfSbufReadBuffer` 反序列化结果。
- **复杂分支**：少分配仅在长度特定位满足时触发。

---

### BO-5：扫描结果 beacon IE 少分配

| 项目 | 内容 |
|------|------|
| **文件** | `sbuf_event_adapter.c` |
| **函数** | `FillScanResult` |
| **行号** | 155–166 |
| **缺陷类型** | 缓冲区溢出（堆越界） |
| **敏感维度** | 跨函数尺寸不一致、复杂运算 |

**缺陷植入逻辑：**

当 `src->beaconIeLen & 0x20U` 成立时，`dst->beaconIe` 按 `src->beaconIeLen - 1` 分配，但后续仍按 `src->beaconIeLen` 复制。

```c
uint32_t beaconAllocLen = src->beaconIeLen;
if (((src->beaconIeLen & 0x20U) != 0) && src->beaconIeLen > 1) {
    beaconAllocLen = src->beaconIeLen - 1;  // 少分配 1 字节
}
dst->beaconIe = OsalMemCalloc(beaconAllocLen);
memcpy_s(dst->beaconIe, src->beaconIeLen, src->beaconIe, src->beaconIeLen);
```

- **跨函数尺寸不一致**：分配长度与拷贝长度来自不同表达式。
- **复杂运算**：少分配由 `beaconIeLen` 特定位触发。

---

### BO-6：接口名数组少分配

| 项目 | 内容 |
|------|------|
| **文件** | `sbuf_cmd_adapter.c` |
| **函数** | `GetIfNames` |
| **行号** | 621–649 |
| **缺陷类型** | 缓冲区溢出（堆越界） |
| **敏感维度** | 循环边界、域敏感 |

**缺陷植入逻辑：**

当 `*num & 0x20U` 成立时按 `*num - 1` 个接口名分配，但循环仍按 `*num` 个元素写入。

```c
uint32_t allocNum = *num;
if (((*num & 0x20U) != 0) && *num > 0) {
    allocNum = *num - 1;  // 少分配 1 个接口名槽位
}
*ifNames = (char *)calloc(allocNum, IFNAMSIZ);
for (i = 0; i < *num; i++) {  // 仍按 *num 写入
    memcpy_s(*ifNames + i * IFNAMSIZ, ...);
}
```

- **循环边界**：分配元素数与循环上界不一致。
- **域敏感**：触发条件由 `*num` 特定位控制。

---

### BO-7：服务释放 trace 缓冲尺寸不一致

| 项目 | 内容 |
|------|------|
| **文件** | `sbuf_common_adapter.c` |
| **函数** | `ReleaseWifiService` |
| **行号** | 51–66 |
| **缺陷类型** | 缓冲区溢出（堆越界） |
| **敏感维度** | 尺寸计算不一致、字段条件 |

**缺陷植入逻辑：**

`releaseTrace` 只按 `sizeof(uint32_t)` 分配，却在 `Dispatch == NULL` 路径中以 `sizeof(uint64_t)` 尺寸写入。

```c
releaseTrace = (char *)malloc(sizeof(uint32_t));
char releaseWord[sizeof(uint64_t)] = {0};
...
if (g_wifiService->dispatcher->Dispatch == NULL) {
    memcpy_s(releaseTrace, sizeof(releaseWord), releaseWord, sizeof(releaseWord));
}
```

- **尺寸计算不一致**：分配尺寸与写入尺寸不匹配。
- **字段敏感**：越界写入由 `Dispatch` 是否为空触发。

---

### BO-8：命令发送 trace 缓冲尺寸不一致

| 项目 | 内容 |
|------|------|
| **文件** | `sbuf_common_adapter.c` |
| **函数** | `SendCmdSync` |
| **行号** | 92–105 |
| **缺陷类型** | 缓冲区溢出（堆越界） |
| **敏感维度** | 尺寸计算不一致、复杂分支 |

**缺陷植入逻辑：**

`cmdTrace` 只按 `sizeof(uint32_t)` 分配，但在 `cmd & 0x20U` 路径下按 `sizeof(uint64_t)` 写入。

```c
cmdTrace = (char *)malloc(sizeof(uint32_t));
char traceWord[sizeof(uint64_t)] = {0};
...
if ((cmd & 0x20U) != 0) {
    memcpy_s(cmdTrace, sizeof(traceWord), traceWord, sizeof(traceWord));
}
```

- **尺寸计算不一致**：分配尺寸与 `memcpy_s` 目标容量不匹配。
- **复杂分支**：越界写入由 `cmd` 特定位触发。

---

## 四、Uninitialized Use（未初始化使用）

### UI-1：回调槽匹配后的事件掩码条件初始化缺失

| 项目 | 内容 |
|------|------|
| **文件** | `wifi_driver_client.c` |
| **函数** | `CountCallbackSlots` + `BuildEventMask` + `WifiEventReport` |
| **行号** | 37–68 |
| **缺陷类型** | 未初始化使用 |
| **敏感维度** | 跨函数、流敏感 |

**缺陷植入逻辑：**

`WifiEventReport` 声明局部变量 `eventMask` 后交给 `BuildEventMask` 初始化。优化后，掩码构造先通过 `CountCallbackSlots()` 确认当前接口确实存在注册槽位，再在事件号有效时写入输出参数；当接口存在回调但 `event` 越界时，`eventMask` 未被赋值，后续回调匹配表达式仍读取该值。

```c
uint32_t eventMask;
BuildEventMask(ifName, event, &eventMask);
...
((eventMask & g_callbackEventMap[i]->eventType) != 0)
```

- **跨函数**：注册槽位统计、掩码构造和回调分发分布在不同函数中。
- **流敏感**：接口注册状态和 `event` 取值共同决定输出参数是否被赋值。

---

### UI-2：EAPOL 发送 trace 状态未初始化日志读取

| 项目 | 内容 |
|------|------|
| **文件** | `sbuf_wpa_cmd_adapter.c` |
| **函数** | `BuildWpaCmdTrace` + `WifiEapolPacketSend` |
| **行号** | 29–40、81–96 |
| **缺陷类型** | 未初始化使用 |
| **敏感维度** | 跨函数、流敏感 |

**缺陷植入逻辑：**

发送路径改为构造 `WpaCmdTrace`。`BuildWpaCmdTrace()` 总是写入 `flags`，但只在 trace 低位为 0 时写入 `status`；当 `length` 为奇数时，调用方根据 `flags` 进入日志分支并读取未初始化的 `sendTrace.status`。

```c
struct WpaCmdTrace sendTrace;
BuildWpaCmdTrace(length, 0x3U, &sendTrace);
if ((sendTrace.flags & 0x1U) != 0) {
    HDF_LOGD("%s: cached eapol ret=%d", __FUNCTION__, sendTrace.status);
}
```

- **跨函数**：trace 结构字段由辅助函数部分初始化，读取发生在发送函数。
- **域敏感**：`flags` 已初始化但 `status` 可能未初始化，缺陷落在结构体字段上。

---

### UI-3：EAPOL 接收 trace 状态缓存未初始化

| 项目 | 内容 |
|------|------|
| **文件** | `sbuf_wpa_cmd_adapter.c` |
| **函数** | `BuildWpaCmdTrace` + `CacheWpaTraceStatus` + `WifiEapolPacketReceive` |
| **行号** | 29–47、147–157 |
| **缺陷类型** | 未初始化使用 |
| **敏感维度** | 跨函数、流敏感 |

**缺陷植入逻辑：**

接收路径同样使用 `WpaCmdTrace`。当接口名首字节为奇数时，`recvTrace.status` 未初始化；调用方不再直接打印该字段，而是调用 `CacheWpaTraceStatus()` 将状态字段复制到本地 trace 缓冲，形成一次未初始化字段读取。

```c
struct WpaCmdTrace recvTrace;
uint8_t recvStatusBytes[sizeof(int32_t)];
BuildWpaCmdTrace((uint32_t)(uint8_t)ifName[0], 0x7U, &recvTrace);
if ((recvTrace.flags & 0x1U) != 0) {
    CacheWpaTraceStatus(&recvTrace, recvStatusBytes, sizeof(recvStatusBytes));
}
```

- **跨函数**：状态字段在构造函数中条件初始化，在缓存函数中被读取。
- **字段敏感**：同一 trace 结构中 `flags` 与 `status` 的初始化状态不同。

---

### UI-4：MAC 查询 trace 状态缓存未初始化

| 项目 | 内容 |
|------|------|
| **文件** | `sbuf_wpa_cmd_adapter.c` |
| **函数** | `BuildWpaCmdTrace` + `CacheWpaTraceStatus` + `WifiCmdGetOwnMac` |
| **行号** | 29–47、432–443 |
| **缺陷类型** | 未初始化使用 |
| **敏感维度** | 跨函数、流敏感 |

**缺陷植入逻辑：**

MAC 查询路径根据 `len` 构造 `macTrace`。当 `len` 为奇数时，`status` 字段未被写入，但 `flags` 触发状态缓存路径，`CacheWpaTraceStatus()` 从未初始化字段复制到本地缓冲。

```c
struct WpaCmdTrace macTrace;
uint8_t macStatusBytes[sizeof(int32_t)];
BuildWpaCmdTrace(len, 0x5U, &macTrace);
if ((macTrace.flags & 0x1U) != 0) {
    CacheWpaTraceStatus(&macTrace, macStatusBytes, sizeof(macStatusBytes));
}
```

- **跨函数**：trace 构造和状态缓存分离。
- **流敏感**：长度低位决定 `status` 字段是否初始化以及缓存路径是否执行。

---

### UI-5：事件分发 trace 状态未初始化读取

| 项目 | 内容 |
|------|------|
| **文件** | `sbuf_event_adapter.c` |
| **函数** | `BuildEventDispatchTrace` + `OnWiFiEvents` |
| **行号** | 32–43、332–340 |
| **缺陷类型** | 未初始化使用 |
| **敏感维度** | 跨函数、流敏感 |

**缺陷植入逻辑：**

事件分发路径改为构造 `EventDispatchTrace`。辅助函数总是写入 `route`，但只在 `eventId` 为偶数时写入 `status`；当 `eventTrace.route & 0x2U` 成立且 `eventId` 为奇数时，日志读取未初始化的 `eventTrace.status`。

```c
static void BuildEventDispatchTrace(uint32_t eventId, struct EventDispatchTrace *trace)
{
    trace->route = eventId & 0x3U;
    if ((eventId & 0x1U) == 0) {
        trace->status = RET_CODE_SUCCESS;
    }
}
...
struct EventDispatchTrace eventTrace;
BuildEventDispatchTrace(eventId, &eventTrace);
if ((eventTrace.route & 0x2U) != 0) {
    HDF_LOGI("%s: cached event status=%d", __FUNCTION__, eventTrace.status);
}
```

- **跨函数**：初始化承诺发生在辅助函数中，读取发生在调用方。
- **域敏感**：`route` 字段已初始化，`status` 字段依赖事件号低位条件初始化。

---

### UI-6：ResetDriver trace 状态未初始化读取

| 项目 | 内容 |
|------|------|
| **文件** | `sbuf_cmd_adapter.c` |
| **函数** | `BuildResetRequestTrace` + `SetResetDriver` |
| **行号** | 33–44、706–708 |
| **缺陷类型** | 未初始化使用 |
| **敏感维度** | 跨函数、流敏感 |

**缺陷植入逻辑：**

ResetDriver 路径通过 `ResetRequestTrace` 记录 chip 路由。`BuildResetRequestTrace()` 只在 `chipId` 为偶数时写入 `status`，但调用方在 `chipRoute & 0x2U` 成立时记录该字段；`chipId == 3` 一类输入会触发未初始化读取。

```c
BuildResetRequestTrace(chipId, &resetTrace);
if ((resetTrace.chipRoute & 0x2U) != 0) {
    HDF_LOGI("%s: cached reset status=%d", __FUNCTION__, resetTrace.status);
}
```

- **跨函数**：被调用函数条件跳过输出参数赋值。
- **域敏感**：读取条件依赖已初始化的 `chipRoute` 字段，缺陷字段为 `status`。

---

### UI-7：服务释放状态缓存未初始化

| 项目 | 内容 |
|------|------|
| **文件** | `sbuf_common_adapter.c` |
| **函数** | `BuildCommonStatusTrace` + `CacheCommonStatusTrace` + `ReleaseWifiService` |
| **行号** | 30–46、64–72 |
| **缺陷类型** | 未初始化使用 |
| **敏感维度** | 字段敏感、跨函数 |

**缺陷植入逻辑：**

服务释放路径通过 `CommonStatusTrace` 记录 dispatcher 状态。`dispatcher != NULL` 时 token 为 1，辅助函数只写入 `route` 而不写入 `status`；调用方随后把 `releaseTraceInfo.status` 缓存到 `releaseTrace`，形成未初始化字段复制。

```c
BuildCommonStatusTrace((uint32_t)(g_wifiService->dispatcher != NULL), 0x1U, &releaseTraceInfo);
if (releaseTraceInfo.route != 0) {
    CacheCommonStatusTrace(&releaseTraceInfo, releaseTrace, sizeof(uint32_t));
}
```

- **字段敏感**：token 取值依赖 `dispatcher` 是否存在。
- **跨函数**：状态构造和状态缓存分离，读取隐藏在缓存复制函数中。

---

### UI-8：命令发送状态缓存未初始化

| 项目 | 内容 |
|------|------|
| **文件** | `sbuf_common_adapter.c` |
| **函数** | `BuildCommonStatusTrace` + `CacheCommonStatusTrace` + `SendCmdSync` |
| **行号** | 30–46、112–114 |
| **缺陷类型** | 未初始化使用 |
| **敏感维度** | 复杂分支、跨函数 |

**缺陷植入逻辑：**

命令发送路径根据 `cmd` 构造 `cmdTraceInfo`。`cmd` 为奇数时 `status` 未初始化；当 `cmdTraceInfo.route & 0x2U` 成立时，调用方把该状态字段复制到 `cmdTrace` 缓冲中，读取条件与初始化条件相互独立。

```c
BuildCommonStatusTrace(cmd, 0x3U, &cmdTraceInfo);
if ((cmdTraceInfo.route & 0x2U) != 0) {
    CacheCommonStatusTrace(&cmdTraceInfo, cmdTrace, sizeof(uint32_t));
}
```

- **跨函数**：状态字段在辅助函数中条件初始化，在缓存函数中读取。
- **复杂分支**：`route` 掩码与 `cmd` 奇偶初始化条件交叉，分析器需要跟踪字段级状态。

---

## 五、Use After Free（使用后释放）

### UAF-1：注册失败清理后读取回调字段

| 项目 | 内容 |
|------|------|
| **文件** | `wifi_driver_client.c` |
| **函数** | `WifiRegisterEventCallback` |
| **行号** | 103–106 |
| **缺陷类型** | 使用后释放（UAF） |
| **敏感维度** | 流敏感、域敏感 |

**缺陷植入逻辑：**

当回调表已满且 `eventType & 0x20000000U` 成立时，代码先释放 `callbackEvent`，随后继续读取 `callbackEvent->ifName[0]` 用于日志。

```c
if ((eventType & 0x20000000U) != 0) {
    free(callbackEvent);
    HDF_LOGE("%s: pending callback for %c", __FUNCTION__, callbackEvent->ifName[0]);
    return RET_CODE_FAILURE;
}
```

- **流敏感**：只在全局表满后的错误分支触发。
- **域敏感**：释放后读取结构体成员字段。

---

### UAF-2：注销回调释放后读取事件类型

| 项目 | 内容 |
|------|------|
| **文件** | `wifi_driver_client.c` |
| **函数** | `WifiUnregisterEventCallback` |
| **行号** | 125–130 |
| **缺陷类型** | 使用后释放（UAF） |
| **敏感维度** | 流敏感、域敏感 |

**缺陷植入逻辑：**

命中待注销回调后，代码先释放 `g_callbackEventMap[i]`。植入后，当 `eventType & 0x04000000U` 成立时，继续读取已释放对象的 `eventType` 字段。

```c
free(g_callbackEventMap[i]);
...
if ((eventType & 0x04000000U) != 0) {
    HDF_LOGI("%s: removed callback %u", __FUNCTION__, g_callbackEventMap[i]->eventType);
}
```

- **流敏感**：必须先匹配并释放全局表项。
- **域敏感**：释放后通过全局表中的结构体成员访问。

---

### UAF-3：EAPOL 镜像缓冲释放后读取

| 项目 | 内容 |
|------|------|
| **文件** | `sbuf_wpa_cmd_adapter.c` |
| **函数** | `WifiEapolPacketSend` |
| **行号** | 85–90 |
| **缺陷类型** | 使用后释放（UAF） |
| **敏感维度** | 流敏感、复杂分支 |

**缺陷植入逻辑：**

`mirror` 在尾部释放后，若 `length & 0x20000000U` 成立且指针非空，继续读取 `mirror[0]`。

```c
free(mirror);
if ((length & 0x20000000U) != 0 && mirror != NULL) {
    HDF_LOGD("%s: mirror first byte=%u", __FUNCTION__, mirror[0]);
}
```

- **流敏感**：读取发生在释放之后。
- **复杂分支**：释放后读取由长度字段高位触发。

---

### UAF-4：EAPOL 接收缓冲释放后读取

| 项目 | 内容 |
|------|------|
| **文件** | `sbuf_wpa_cmd_adapter.c` |
| **函数** | `WifiEapolPacketReceive` |
| **行号** | 153–157 |
| **缺陷类型** | 使用后释放（UAF） |
| **敏感维度** | 流敏感、跨函数 |

**缺陷植入逻辑：**

当 `memcpy_s` 失败时，先释放 `rxEapol->buf`，随后在错误日志中读取 `rxEapol->buf[0]`。

```c
if (memcpy_s(rxEapol->buf, eapol.len, eapol.buf, eapol.len) != EOK) {
    free(rxEapol->buf);
    free(rxEapol->buf);
    HDF_LOGE("%s: rx first byte=%u", __FUNCTION__, rxEapol->buf[0]);
}
```

- **流敏感**：只在拷贝失败分支触发。
- **跨函数**：源数据和长度来自 SBuf 反序列化结果。

---

### UAF-5：扫描结果 IE 跨函数释放后读取

| 项目 | 内容 |
|------|------|
| **文件** | `sbuf_event_adapter.c` |
| **函数** | `FillScanResult` |
| **行号** | 174–177 |
| **缺陷类型** | 使用后释放（UAF） |
| **敏感维度** | 跨文件、跨函数、流敏感 |

**缺陷植入逻辑：**

错误路径调用 `FreeScanResult(dst)` 后，在 `src->ieLen & 0x20000000U` 条件下继续读取 `dst->ie[0]`。

```c
FreeScanResult(dst);
if (((src->ieLen & 0x20000000U) != 0) && dst->ie != NULL) {
    HDF_LOGE("%s: stale ie first byte=%u", __FUNCTION__, dst->ie[0]);
}
```

- **跨文件**：`FreeScanResult()` 定义在 `wifi_driver_client.c`。
- **跨函数**：释放与读取分属不同清理层次。
- **流敏感**：读取发生在统一清理之后。

---

### UAF-6：接口名数组释放后读取

| 项目 | 内容 |
|------|------|
| **文件** | `sbuf_cmd_adapter.c` |
| **函数** | `GetIfNames` |
| **行号** | 637–643 |
| **缺陷类型** | 使用后释放（UAF） |
| **敏感维度** | 流敏感、复杂分支 |

**缺陷植入逻辑：**

`free(*ifNames)` 后，若 `*num & 0x20000000U` 成立继续读取 `(*ifNames)[0]`。

```c
free(*ifNames);
...
if ((*num & 0x20000000U) != 0) {
    HDF_LOGE("%s: stale ifname first byte=%d", __FUNCTION__, (*ifNames)[0]);
}
```

- **流敏感**：读取发生在释放之后。
- **复杂分支**：释放后读取由 `*num` 高位标志控制。

---

### UAF-7：服务释放 trace 释放后读取

| 项目 | 内容 |
|------|------|
| **文件** | `sbuf_common_adapter.c` |
| **函数** | `ReleaseWifiService` |
| **行号** | 67–70 |
| **缺陷类型** | 使用后释放（UAF） |
| **敏感维度** | 字段敏感、释放后日志路径 |

**缺陷植入逻辑：**

`releaseTrace` 被释放后，如果 `Dispatch != NULL`，继续读取 `releaseTrace[0]` 写日志。

```c
free(releaseTrace);
if (g_wifiService->dispatcher->Dispatch != NULL) {
    HDF_LOGI("%s: release trace first byte=%d", __FUNCTION__, releaseTrace[0]);
}
```

- **字段敏感**：释放后读取路径依赖 `Dispatch` 是否非空。
- **释放后日志路径**：UAF 发生在日志记录语句中。

---

### UAF-8：命令发送 trace 释放后读取

| 项目 | 内容 |
|------|------|
| **文件** | `sbuf_common_adapter.c` |
| **函数** | `SendCmdSync` |
| **行号** | 108–114 |
| **缺陷类型** | 使用后释放（UAF） |
| **敏感维度** | 流敏感、复杂分支 |

**缺陷植入逻辑：**

`cmdTrace` 被释放后，若 `cmd & 0x20000000U` 成立继续读取 `cmdTrace[0]`。

```c
free(cmdTrace);
if ((cmd & 0x20000000U) != 0) {
    HDF_LOGI("%s: cmd trace first byte=%d", __FUNCTION__, cmdTrace[0]);
}
```

- **流敏感**：读取发生在释放之后。
- **复杂分支**：释放后读取由 `cmd` 高位标志控制。

---

## 缺陷分布汇总

| 编号 | 缺陷类型 | 文件 | 函数 | 敏感维度 |
|------|----------|------|------|----------|
| ML-1 | 内存泄漏 | wifi_driver_client.c | WifiRegisterEventCallback | 流敏感+域敏感 |
| ML-2 | 内存泄漏 | wifi_driver_client.c | WifiRegisterHid2dCallback | 流敏感+复杂分支 |
| ML-3 | 内存泄漏 | sbuf_wpa_cmd_adapter.c | WifiEapolPacketSend | 流敏感+复杂运算 |
| ML-4 | 内存泄漏 | sbuf_wpa_cmd_adapter.c | WifiCmdGetOwnMac | 流敏感+复杂运算 |
| ML-5 | 内存泄漏 | sbuf_event_adapter.c | FillScanResult | 跨函数+流敏感 |
| ML-6 | 内存泄漏 | sbuf_cmd_adapter.c | GetIfNames | 流敏感+域敏感 |
| ML-7 | 内存泄漏 | sbuf_common_adapter.c | ReleaseWifiService | 字段敏感+流敏感 |
| ML-8 | 内存泄漏 | sbuf_common_adapter.c | SendCmdSync | 流敏感+复杂分支 |
| DF-1 | 双重释放 | wifi_driver_client.c | WifiRegisterEventCallback | 流敏感 |
| DF-2 | 双重释放 | wifi_driver_client.c | WifiUnregisterEventCallback | 流敏感+域敏感 |
| DF-3 | 双重释放 | sbuf_wpa_cmd_adapter.c | WifiEapolPacketSend | 流敏感+复杂分支 |
| DF-4 | 双重释放 | sbuf_wpa_cmd_adapter.c | WifiEapolPacketReceive | 流敏感+跨函数 |
| DF-5 | 双重释放 | sbuf_event_adapter.c | FillScanResult | 跨文件+跨函数 |
| DF-6 | 双重释放 | sbuf_cmd_adapter.c | GetIfNames | 流敏感+复杂分支 |
| DF-7 | 双重释放 | sbuf_common_adapter.c | ReleaseWifiService | 流敏感+尾部清理 |
| DF-8 | 双重释放 | sbuf_common_adapter.c | SendCmdSync | 流敏感+复杂分支 |
| BO-1 | 缓冲区溢出 | wifi_driver_client.c | WifiRegisterEventCallback | 流敏感+复杂运算 |
| BO-2 | 缓冲区溢出 | wifi_driver_client.c | WifiRegisterHid2dCallback | 域敏感+复杂运算 |
| BO-3 | 缓冲区溢出 | sbuf_wpa_cmd_adapter.c | WifiEapolPacketSend | 流敏感+复杂运算 |
| BO-4 | 缓冲区溢出 | sbuf_wpa_cmd_adapter.c | WifiEapolPacketReceive | 跨函数+复杂分支 |
| BO-5 | 缓冲区溢出 | sbuf_event_adapter.c | FillScanResult | 尺寸不一致+跨函数 |
| BO-6 | 缓冲区溢出 | sbuf_cmd_adapter.c | GetIfNames | 循环边界+域敏感 |
| BO-7 | 缓冲区溢出 | sbuf_common_adapter.c | ReleaseWifiService | 尺寸不一致+字段条件 |
| BO-8 | 缓冲区溢出 | sbuf_common_adapter.c | SendCmdSync | 尺寸不一致+复杂分支 |
| UI-1 | 未初始化使用 | wifi_driver_client.c | CountCallbackSlots + BuildEventMask + WifiEventReport | 跨函数+流敏感 |
| UI-2 | 未初始化使用 | sbuf_wpa_cmd_adapter.c | BuildWpaCmdTrace + WifiEapolPacketSend | 跨函数+字段敏感 |
| UI-3 | 未初始化使用 | sbuf_wpa_cmd_adapter.c | BuildWpaCmdTrace + CacheWpaTraceStatus + WifiEapolPacketReceive | 跨函数+字段敏感 |
| UI-4 | 未初始化使用 | sbuf_wpa_cmd_adapter.c | BuildWpaCmdTrace + CacheWpaTraceStatus + WifiCmdGetOwnMac | 跨函数+字段敏感 |
| UI-5 | 未初始化使用 | sbuf_event_adapter.c | BuildEventDispatchTrace + OnWiFiEvents | 跨函数+字段敏感 |
| UI-6 | 未初始化使用 | sbuf_cmd_adapter.c | BuildResetRequestTrace + SetResetDriver | 跨函数+字段敏感 |
| UI-7 | 未初始化使用 | sbuf_common_adapter.c | BuildCommonStatusTrace + CacheCommonStatusTrace + ReleaseWifiService | 字段敏感+跨函数 |
| UI-8 | 未初始化使用 | sbuf_common_adapter.c | BuildCommonStatusTrace + CacheCommonStatusTrace + SendCmdSync | 跨函数+复杂分支 |
| UAF-1 | 使用后释放 | wifi_driver_client.c | WifiRegisterEventCallback | 流敏感+域敏感 |
| UAF-2 | 使用后释放 | wifi_driver_client.c | WifiUnregisterEventCallback | 流敏感+域敏感 |
| UAF-3 | 使用后释放 | sbuf_wpa_cmd_adapter.c | WifiEapolPacketSend | 流敏感+复杂分支 |
| UAF-4 | 使用后释放 | sbuf_wpa_cmd_adapter.c | WifiEapolPacketReceive | 流敏感+跨函数 |
| UAF-5 | 使用后释放 | sbuf_event_adapter.c | FillScanResult | 跨文件+跨函数 |
| UAF-6 | 使用后释放 | sbuf_cmd_adapter.c | GetIfNames | 流敏感+复杂分支 |
| UAF-7 | 使用后释放 | sbuf_common_adapter.c | ReleaseWifiService | 字段敏感+释放后日志 |
| UAF-8 | 使用后释放 | sbuf_common_adapter.c | SendCmdSync | 流敏感+复杂分支 |

**按文件分布：**
- `wifi_driver_client.c`：9 个，ML-1, ML-2, DF-1, DF-2, BO-1, BO-2, UI-1, UAF-1, UAF-2
- `sbuf_wpa_cmd_adapter.c`：11 个，ML-3, ML-4, DF-3, DF-4, BO-3, BO-4, UI-2, UI-3, UI-4, UAF-3, UAF-4
- `sbuf_event_adapter.c`：5 个，ML-5, DF-5, BO-5, UI-5, UAF-5
- `sbuf_cmd_adapter.c`：5 个，ML-6, DF-6, BO-6, UI-6, UAF-6
- `sbuf_common_adapter.c`：10 个，ML-7, ML-8, DF-7, DF-8, BO-7, BO-8, UI-7, UI-8, UAF-7, UAF-8

---

## 静态分析检测结果

### 按植入缺陷统计命中

| 分析器 | ML | DF | BO | UI | UAF | 合计命中 |
|--------|----|----|----|----|-----|----------|
| Cppcheck | 3/8 | 4/8 | 2/8 | 0/8 | 4/8 | 13/40 |
| Infer | 4/8 | 0/8 | 0/8 | 4/8 | 1/8 | 9/40 |
| CSA | 3/8 | 4/8 | 0/8 | 4/8 | 3/8 | 14/40 |
| SVF | 6/8 | 6/8 | 2/8 | 8/8 | 6/8 | 28/40 |

### 逐项命中

| 编号 | 敏感维度 | 文件 | 本项目 | Cppcheck | Infer | CSA |
|------|----------|------|--------|----------|-------|-----|
| ML-1 | 流敏感+域敏感 | wifi_driver_client.c | ✅ | ❌ | ❌ | ❌ |
| ML-2 | 流敏感+复杂分支 | wifi_driver_client.c | ❌ | ❌ | ❌ | ❌ |
| ML-3 | 流敏感+复杂运算 | sbuf_wpa_cmd_adapter.c | ✅ | ✅ | ✅ | ❌ |
| ML-4 | 流敏感+复杂运算 | sbuf_wpa_cmd_adapter.c | ✅ | ✅ | ✅ | ✅ |
| ML-5 | 跨函数+流敏感 | sbuf_event_adapter.c | ❌ | ❌ | ❌ | ❌ |
| ML-6 | 流敏感+域敏感 | sbuf_cmd_adapter.c | ✅ | ❌ | ❌ | ❌ |
| ML-7 | 字段敏感+流敏感 | sbuf_common_adapter.c | ✅ | ❌ | ✅ | ✅ |
| ML-8 | 流敏感+复杂分支 | sbuf_common_adapter.c | ✅ | ✅ | ✅ | ✅ |
| DF-1 | 流敏感 | wifi_driver_client.c | ✅ | ❌ | ❌ | ❌ |
| DF-2 | 流敏感+域敏感 | wifi_driver_client.c | ✅ | ❌ | ❌ | ❌ |
| DF-3 | 流敏感+复杂分支 | sbuf_wpa_cmd_adapter.c | ✅ | ✅ | ❌ | ❌ |
| DF-4 | 流敏感+跨函数 | sbuf_wpa_cmd_adapter.c | ✅ | ✅ | ❌ | ✅ |
| DF-5 | 跨文件+跨函数 | sbuf_event_adapter.c | ❌ | ❌ | ❌ | ❌ |
| DF-6 | 流敏感+复杂分支 | sbuf_cmd_adapter.c | ❌ | ❌ | ❌ | ❌ |
| DF-7 | 流敏感+尾部清理 | sbuf_common_adapter.c | ✅ | ✅ | ❌ | ✅ |
| DF-8 | 流敏感+复杂分支 | sbuf_common_adapter.c | ✅ | ✅ | ❌ | ✅ |
| BO-1 | 流敏感+复杂运算 | wifi_driver_client.c | ✅ | ✅ | ❌ | ❌ |
| BO-2 | 域敏感+复杂运算 | wifi_driver_client.c | ❌ | ❌ | ❌ | ❌ |
| BO-3 | 流敏感+复杂运算 | sbuf_wpa_cmd_adapter.c | ✅ | ✅ | ❌ | ❌ |
| BO-4 | 跨函数+复杂分支 | sbuf_wpa_cmd_adapter.c | ❌ | ❌ | ❌ | ❌ |
| BO-5 | 尺寸不一致+跨函数 | sbuf_event_adapter.c | ❌ | ❌ | ❌ | ❌ |
| BO-6 | 循环边界+域敏感 | sbuf_cmd_adapter.c | ❌ | ❌ | ❌ | ❌ |
| BO-7 | 尺寸不一致+字段条件 | sbuf_common_adapter.c | ❌ | ❌ | ❌ | ❌ |
| BO-8 | 尺寸不一致+复杂分支 | sbuf_common_adapter.c | ❌ | ❌ | ❌ | ❌ |
| UI-1 | 跨函数+流敏感 | wifi_driver_client.c | ✅ | ❌ | ✅ | ✅ |
| UI-2 | 跨函数+字段敏感 | sbuf_wpa_cmd_adapter.c | ✅ | ❌ | ✅ | ✅ |
| UI-3 | 跨函数+字段敏感 | sbuf_wpa_cmd_adapter.c | ✅ | ❌ | ❌ | ❌ |
| UI-4 | 跨函数+字段敏感 | sbuf_wpa_cmd_adapter.c | ✅ | ❌ | ❌ | ❌ |
| UI-5 | 跨函数+字段敏感 | sbuf_event_adapter.c | ✅ | ❌ | ✅ | ✅ |
| UI-6 | 跨函数+字段敏感 | sbuf_cmd_adapter.c | ✅ | ❌ | ✅ | ✅ |
| UI-7 | 字段敏感+跨函数 | sbuf_common_adapter.c | ✅ | ❌ | ❌ | ❌ |
| UI-8 | 跨函数+复杂分支 | sbuf_common_adapter.c | ✅ | ❌ | ❌ | ❌ |
| UAF-1 | 流敏感+域敏感 | wifi_driver_client.c | ✅ | ❌ | ❌ | ❌ |
| UAF-2 | 流敏感+域敏感 | wifi_driver_client.c | ✅ | ❌ | ❌ | ❌ |
| UAF-3 | 流敏感+复杂分支 | sbuf_wpa_cmd_adapter.c | ✅ | ✅ | ❌ | ❌ |
| UAF-4 | 流敏感+跨函数 | sbuf_wpa_cmd_adapter.c | ✅ | ✅ | ✅ | ❌ |
| UAF-5 | 跨文件+跨函数 | sbuf_event_adapter.c | ❌ | ❌ | ❌ | ❌ |
| UAF-6 | 流敏感+复杂分支 | sbuf_cmd_adapter.c | ❌ | ❌ | ❌ | ✅ |
| UAF-7 | 字段敏感+释放后日志 | sbuf_common_adapter.c | ✅ | ✅ | ❌ | ✅ |
| UAF-8 | 流敏感+复杂分支 | sbuf_common_adapter.c | ✅ | ✅ | ❌ | ✅ |
