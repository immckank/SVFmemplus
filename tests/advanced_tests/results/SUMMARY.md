# BOF 困难版测试结果汇总

- 生成时间: 2026-06-05 23:12:48
- 检测器: `/home/cloud/Project/SVFmemplus/Release-build/bin/bof`
- 说明: `bof` 仅检测 BUFFER_OVERFLOW(BO) 类缺陷；ML/UAF/DF/UU 不在职责范围，不计入 ground truth。
- 本表统计 **检测到的 MUST/MAY 报告数** 与 **注入的 BO 缺陷数(期望)** 的对照；困难版以收集+分析为目标，不因漏报判失败。

| 用例 | 期望BO | MUST | MAY | 检测合计 | 覆盖率(检测/期望) | 来源 |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| object1_kernel_50 | 10 | 13 | 11 | 24 | 240% | kernel sentry driver, 50 defects (BO: DEFECT-2,7,11,16,21,27,32,37,42,47) |
| object3_ubs_50 | 10 | 16 | 0 | 16 | 160% | ubs_engine, 50 defects (BO: DEFECT-2,7,11,16,21,27,32,37,42,47) |
| object1_kernel_10 | 2 | 2 | 67 | 69 | 3450% | kernel sentry driver subset, 10 defects (BO-1,BO-2) |
| object3_ubs_10 | 2 | 68 | 115 | 183 | 9150% | ubs_engine sdk subset, 10 defects (BO-1,BO-2) |
| openharmony_40 | 8 | 2 | 4 | 6 | 75% | OpenHarmony wlan client, 40 defects (BO-1..BO-8) |
| **合计** | **32** | | | **298** | | |

## 各用例检测到的越界位置（line/file）

### object1_kernel_50

```
[BufferOverflowChecker] start buffer overflow analysis
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 374, "col": 17, "file": "focused_src/sentry_remote_client_defects.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 375, "col": 3, "file": "focused_src/sentry_remote_client_defects.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 422, "col": 17, "file": "focused_src/sentry_remote_client_defects.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 423, "col": 3, "file": "focused_src/sentry_remote_client_defects.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 481, "col": 3, "file": "focused_src/sentry_remote_client_defects.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 482, "col": 7, "file": "focused_src/sentry_remote_client_defects.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 483, "col": 13, "file": "focused_src/sentry_remote_client_defects.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 488, "col": 9, "file": "focused_src/sentry_remote_client_defects.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 637, "col": 17, "file": "focused_src/sentry_remote_client_defects.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 638, "col": 3, "file": "focused_src/sentry_remote_client_defects.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 132, "col": 2, "file": "focused_src/sentry_remote_client_defects.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 206, "col": 2, "file": "focused_src/sentry_remote_client_defects.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 46, "col": 16, "file": "arch/arm64/include/asm/uaccess.h" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 360, "col": 2, "file": "focused_src/sentry_remote_client_defects.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 519, "col": 2, "file": "focused_src/sentry_remote_client_defects.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 715, "col": 2, "file": "focused_src/sentry_remote_client_defects.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 823, "col": 2, "file": "focused_src/sentry_remote_client_defects.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 886, "col": 2, "file": "focused_src/sentry_remote_client_defects.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 950, "col": 2, "file": "focused_src/sentry_remote_client_defects.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 1013, "col": 2, "file": "focused_src/sentry_remote_client_defects.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 1080, "col": 2, "file": "focused_src/sentry_remote_client_defects.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 153, "col": 13, "file": "include/linux/uaccess.h" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : CallICFGNode: { "line": 153, "col": 3, "file": "include/linux/uaccess.h" }
[BufferOverflowChecker] MUST buffer overflow (memcpy/memmove)
  Location   : CallICFGNode: { "line": 483, "col": 4, "file": "focused_src/sentry_remote_client_defects.c" }
```

### object3_ubs_50

```
[BufferOverflowChecker] start buffer overflow analysis
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 58, "col": 5, "file": "focused_src/ubs_engine_mem_defects.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 186, "col": 5, "file": "focused_src/ubs_engine_mem_defects.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 260, "col": 5, "file": "focused_src/ubs_engine_mem_defects.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 253, "col": 5, "file": "focused_src/ubs_engine_mem_defects.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 406, "col": 5, "file": "focused_src/ubs_engine_mem_defects.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 529, "col": 5, "file": "focused_src/ubs_engine_mem_defects.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 522, "col": 5, "file": "focused_src/ubs_engine_mem_defects.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 610, "col": 5, "file": "focused_src/ubs_engine_mem_defects.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 660, "col": 5, "file": "focused_src/ubs_engine_mem_defects.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 721, "col": 5, "file": "focused_src/ubs_engine_mem_defects.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 784, "col": 5, "file": "focused_src/ubs_engine_mem_defects.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 845, "col": 5, "file": "focused_src/ubs_engine_mem_defects.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 912, "col": 5, "file": "focused_src/ubs_engine_mem_defects.c" }
[BufferOverflowChecker] MUST buffer overflow (memcpy/memmove)
  Location   : CallICFGNode: { "line": 255, "col": 11, "file": "focused_src/ubs_engine_mem_defects.c" }
[BufferOverflowChecker] MUST buffer overflow (memcpy/memmove)
  Location   : CallICFGNode: { "line": 524, "col": 11, "file": "focused_src/ubs_engine_mem_defects.c" }
[BufferOverflowChecker] MUST buffer overflow (memcpy/memmove)
  Location   : CallICFGNode: { "line": 605, "col": 11, "file": "focused_src/ubs_engine_mem_defects.c" }
```

### object1_kernel_10

```
[BufferOverflowChecker] start buffer overflow analysis
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 164, "col": 18, "file": "drivers/ub/sentry/sentry_remote_client.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 192, "col": 21, "file": "drivers/ub/sentry/sentry_remote_client.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 195, "col": 6, "file": "drivers/ub/sentry/sentry_remote_client.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 694, "col": 17, "file": "drivers/ub/sentry/sentry_remote_client.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 695, "col": 3, "file": "drivers/ub/sentry/sentry_remote_client.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 649, "col": 17, "file": "drivers/ub/sentry/sentry_remote_client.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 650, "col": 3, "file": "drivers/ub/sentry/sentry_remote_client.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 571, "col": 17, "file": "drivers/ub/sentry/sentry_remote_client.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 572, "col": 3, "file": "drivers/ub/sentry/sentry_remote_client.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 525, "col": 17, "file": "drivers/ub/sentry/sentry_remote_client.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 526, "col": 3, "file": "drivers/ub/sentry/sentry_remote_client.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 825, "col": 17, "file": "drivers/ub/sentry/sentry_remote_client.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 826, "col": 3, "file": "drivers/ub/sentry/sentry_remote_client.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 828, "col": 34, "file": "drivers/ub/sentry/sentry_remote_client.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 865, "col": 6, "file": "drivers/ub/sentry/sentry_remote_client.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 847, "col": 38, "file": "drivers/ub/sentry/sentry_remote_client.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 352, "col": 17, "file": "drivers/ub/sentry/sentry_reporter.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 353, "col": 3, "file": "drivers/ub/sentry/sentry_reporter.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 286, "col": 17, "file": "drivers/ub/sentry/sentry_reporter.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 287, "col": 3, "file": "drivers/ub/sentry/sentry_reporter.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 220, "col": 17, "file": "drivers/ub/sentry/sentry_reporter.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 221, "col": 3, "file": "drivers/ub/sentry/sentry_reporter.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 418, "col": 17, "file": "drivers/ub/sentry/sentry_reporter.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 419, "col": 3, "file": "drivers/ub/sentry/sentry_reporter.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 1486, "col": 17, "file": "drivers/ub/sentry/sentry_urma_comm.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 1487, "col": 3, "file": "drivers/ub/sentry/sentry_urma_comm.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 1621, "col": 7, "file": "drivers/ub/sentry/sentry_urma_comm.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 1645, "col": 11, "file": "drivers/ub/sentry/sentry_urma_comm.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 1414, "col": 41, "file": "drivers/ub/sentry/sentry_urma_comm.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 1381, "col": 7, "file": "drivers/ub/sentry/sentry_urma_comm.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 1416, "col": 45, "file": "drivers/ub/sentry/sentry_urma_comm.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 147, "col": 5, "file": "drivers/ub/sentry/sentry_uvb_comm.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 102, "col": 32, "file": "drivers/ub/sentry/sentry_uvb_comm.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 108, "col": 8, "file": "drivers/ub/sentry/sentry_uvb_comm.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 123, "col": 27, "file": "drivers/ub/sentry/sentry_uvb_comm.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : CallICFGNode: { "line": 1759, "col": 33, "file": "drivers/ub/sentry/sentry_urma_comm.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : CallICFGNode: { "line": 840, "col": 43, "file": "drivers/ub/sentry/sentry_urma_comm.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : CallICFGNode: { "line": 841, "col": 15, "file": "drivers/ub/sentry/sentry_urma_comm.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 46, "col": 16, "file": "arch/arm64/include/asm/uaccess.h" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
```

### object3_ubs_10

```
[BufferOverflowChecker] start buffer overflow analysis
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 214, "col": 5, "file": "src/sdk/ubs_engine_mem.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 441, "col": 5, "file": "src/sdk/ubs_engine_mem.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 519, "col": 5, "file": "src/sdk/ubs_engine_mem.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 27, "col": 23, "file": "src/sdk/sample/ubse_mem_fd_sample.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 297, "col": 33, "file": "src/sdk/libubse_helper.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 314, "col": 33, "file": "src/sdk/libubse_helper.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 297, "col": 20, "file": "src/sdk/libubse_helper.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 314, "col": 20, "file": "src/sdk/libubse_helper.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 605, "col": 9, "file": "src/sdk/libubse_helper.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 634, "col": 13, "file": "src/sdk/libubse_helper.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 126, "col": 5, "file": "src/sdk/libubse_helper.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 127, "col": 9, "file": "src/sdk/libubse_helper.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 130, "col": 9, "file": "src/sdk/libubse_helper.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 133, "col": 9, "file": "src/sdk/libubse_helper.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 135, "col": 9, "file": "src/sdk/libubse_helper.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 137, "col": 9, "file": "src/sdk/libubse_helper.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 140, "col": 9, "file": "src/sdk/libubse_helper.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 143, "col": 9, "file": "src/sdk/libubse_helper.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 299, "col": 48, "file": "src/sdk/libubse_helper.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 169, "col": 5, "file": "src/sdk/libubse_helper.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 170, "col": 9, "file": "src/sdk/libubse_helper.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 173, "col": 9, "file": "src/sdk/libubse_helper.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 175, "col": 9, "file": "src/sdk/libubse_helper.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 177, "col": 9, "file": "src/sdk/libubse_helper.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 180, "col": 9, "file": "src/sdk/libubse_helper.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 318, "col": 9, "file": "src/sdk/libubse_helper.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 333, "col": 52, "file": "src/sdk/libubse_helper.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 197, "col": 5, "file": "src/sdk/libubse_helper.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 198, "col": 9, "file": "src/sdk/libubse_helper.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 210, "col": 5, "file": "src/sdk/libubse_helper.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 211, "col": 9, "file": "src/sdk/libubse_helper.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 214, "col": 9, "file": "src/sdk/libubse_helper.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 367, "col": 9, "file": "src/sdk/libubse_helper.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 371, "col": 9, "file": "src/sdk/libubse_helper.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 229, "col": 5, "file": "src/sdk/libubse_helper.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 230, "col": 9, "file": "src/sdk/libubse_helper.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 385, "col": 9, "file": "src/sdk/libubse_helper.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 418, "col": 13, "file": "src/sdk/libubse_helper.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Location   : { "line": 506, "col": 9, "file": "src/sdk/libubse_helper.c" }
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
```

### openharmony_40

```
[BufferOverflowChecker] start buffer overflow analysis
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 100, "col": 5, "file": "../../drivers/peripheral/wlan/client/src/sbuf/sbuf_wpa_cmd_adapter.c" }
[BufferOverflowChecker] MAY buffer overflow (array/pointer access)
  Location   : { "line": 98, "col": 5, "file": "../../drivers/peripheral/wlan/client/src/wifi_driver_client.c" }
[BufferOverflowChecker] MAY buffer overflow (memcpy/memmove)
  Location   : CallICFGNode: { "line": 69, "col": 9, "file": "../../drivers/peripheral/wlan/client/src/sbuf/sbuf_wpa_cmd_adapter.c" }
[BufferOverflowChecker] MAY buffer overflow (memcpy/memmove)
  Location   : CallICFGNode: { "line": 170, "col": 13, "file": "../../drivers/peripheral/wlan/client/src/sbuf/sbuf_event_adapter.c" }
[BufferOverflowChecker] MUST buffer overflow (memcpy/memmove)
  Location   : CallICFGNode: { "line": 78, "col": 19, "file": "../../drivers/peripheral/wlan/client/src/sbuf/sbuf_common_adapter.c" }
[BufferOverflowChecker] MUST buffer overflow (memcpy/memmove)
  Location   : CallICFGNode: { "line": 117, "col": 15, "file": "../../drivers/peripheral/wlan/client/src/sbuf/sbuf_common_adapter.c" }
```

