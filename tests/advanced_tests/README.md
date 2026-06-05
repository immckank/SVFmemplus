# BOF 困难版测试用例（真实项目缺陷注入）

本目录提供来自真实项目的**困难版** BOF（缓冲区溢出）测试用例，与 `../basic_tests/`
（最小回归用例）互补。用例为预编译的 LLVM bitcode（`.bc`），来源于内核驱动、
ubs_engine SDK 与 OpenHarmony WLAN 客户端，并由第三方注入了多类内存缺陷。

> **重要**：`bof` 检测器只负责 **BUFFER_OVERFLOW（BO）** 一类缺陷。注入用例中还包含
> MEMORY_LEAK / USE_AFTER_FREE / DOUBLE_FREE / UNINITIALIZED_USED 等其他类别，
> 这些**不在 bof 职责范围内**，不计入本目录的 ground truth。

## 用例清单（cases/）

| 用例目录 | bitcode | 配套源码 | 注入 BO 缺陷数 | 来源 |
| --- | --- | --- | ---: | --- |
| `object1_kernel_50` | `sentry_remote_client_defects.bc` | `sentry_remote_client_defects.c` | 10 | 内核 sentry 驱动，50 缺陷 |
| `object3_ubs_50` | `ubs_engine_mem_defects.with_ipc_free.bc` | `ubs_engine_mem_defects.c` | 10 | ubs_engine，50 缺陷 |
| `object1_kernel_10` | `svf_subset.bc` | 报告 `object1-缺陷植入报告.md` | 2 | 内核 sentry 子集，10 缺陷 |
| `object3_ubs_10` | `sdk_subset.bc` | 报告 `object3-缺陷植入报告.md` | 2 | ubs_engine 子集，10 缺陷 |
| `openharmony_40` | `openharmony_wlan_client_linked.bc` | 报告 `opneharmony-缺陷植入报告.md` | 8 | OpenHarmony WLAN client，40 缺陷 |

### BO ground truth 说明

- **50 缺陷集**（`object1_kernel_50` / `object3_ubs_50`）：BO 缺陷在 `.c` 内以
  `/* DEFECT-N: BUFFER_OVERFLOW ... */` 内联标注，分别为
  `DEFECT-2, 7, 11, 16, 21, 27, 32, 37, 42, 47`（共 10 个）。
- **10 缺陷集**（`object1_kernel_10` / `object3_ubs_10`）：各含 2 个 BO 缺陷（`BO-1`、`BO-2`），
  详见同目录缺陷植入报告 `.md`。
- **openharmony_40**：含 8 个 BO 缺陷（`BO-1` ~ `BO-8`），详见 `opneharmony-缺陷植入报告.md`。

## 运行方法

```bash
./run_hard_tests.sh
```

脚本会自动定位 `bof`（`../../Release-build/bin/bof`），对每个 `.bc` 运行检测器，
统计 `MUST` / `MAY` 越界报告数并与注入 BO 缺陷数对照，结果写入：

- `results/<用例>.out.txt`：逐用例完整检测输出；
- `results/SUMMARY.md`：覆盖率汇总表 + 各用例检测到的越界位置（line/file）。

> `openharmony_40` 的 `.bc` 由该项目 `本项目/bc/` 下 5 个按文件拆分的 bitcode 经 `llvm-link`
> 合并而成（原 `svf_subset.bc` 是不含 BO 缺陷函数的裁剪子集，不可用于 BO 评测）。

详细的**失败根因分析与改进说明**见 [`ANALYSIS.md`](./ANALYSIS.md)。

可用环境变量覆盖工具路径：

```bash
BOF=/path/to/bof ./run_hard_tests.sh
```

> 困难版以"**收集 + 分析**"为目标：脚本**不会**因漏报而返回失败退出码，
> 便于据此分析漏报根因并迭代改进检测器。

## 与简单版的区别

| 维度 | 简单版 (`../basic_tests`) | 困难版 (本目录) |
| --- | --- | --- |
| 输入 | 手写最小 `.c` → clang 现编译 | 真实项目预编译 `.bc` |
| 断言 | 精确 MUST/MAY 基线，失败即 FAIL | 仅收集对照，不阻断 |
| 目标 | 防回归 | 衡量真实场景能力 + 漏报分析 |
