# BOF 模块使用说明

> 本文档面向使用者，说明如何**构建、运行 `bof` 检测器**，如何解读输出，以及如何触发/使用各项特性（堆分配识别、内存拷贝检查、跨函数检测、符号仿射少分配判定、LLM 辅助 MAY-triage）。
> 设计原理见《BOF模块新特性设计文档.md》，技术定位见《BOF模块技术定位与优劣分析.md》。

---

## 一、概述

`bof` 是构建在 SVF（LLVM 16）之上的缓冲区越界 / 数组越界静态检测器，输入为 LLVM IR（`.ll`/`.bc`），输出越界告警，分 **MUST**（必然越界）与 **MAY**（可能越界）两级。当前已落地五项能力：

| 能力 | 作用 | 触发方式 |
| --- | --- | --- |
| GEP / 数组下标越界 | 栈/堆数组下标越界 | 默认，对所有 `getelementptr` 访问生效 |
| 堆分配识别 | 识别 malloc/calloc/realloc 及自定义分配器并推导尺寸 | 默认 |
| 内存拷贝检查 | memcpy/memset/strncpy/strcpy… 写读越界 | 默认，命中规则表的调用 |
| 跨函数检测 | 指针透传到子函数后的越界 | 默认（方案 A，调用深度上限 4） |
| 符号仿射少分配 | `malloc(len-1)+memcpy(len)` 同源 off-by-one | 默认 |
| **LLM 辅助 MAY-triage** | 对循环退化的幸存 MAY 抽切片交大模型分流 | **需显式开启**（见第六节） |

> 前五项是 **sound 核心**，默认全开、无需配置；第六项是**纯叠加 overlay**，默认只在配置后才调用大模型，不改变任何 sound 结论。

---

## 二、构建

### 2.1 首次完整构建

```bash
cd SVFmemplus
./setup.sh        # 准备 LLVM 16 / Z3 等依赖（首次）
./build.sh        # Release 构建；产物在 Release-build/bin/bof
# ./build.sh debug  # 如需调试版，产物在 Debug-build/bin/bof
```

### 2.2 增量构建（日常改 BOF 代码后）

```bash
./build_bof.sh          # 仅重建 SvfCore + bof（Release-build）
./build_bof.sh debug    # Debug 版
JOBS=16 ./build_bof.sh  # 调整并行度
```

> 新增 `.cpp`/`.h` 文件时，`svf/CMakeLists.txt` 用 `GLOB_RECURSE`，需先 `cmake Release-build` 重新配置一次再增量构建。

构建产物：`Release-build/bin/bof`。

---

## 三、准备输入（编译为 LLVM IR）

`bof` 吃 LLVM IR。用 clang 16 生成，**务必带以下 flag**：

```bash
clang -S -emit-llvm -g -O0 -fno-discard-value-names foo.c -o foo.ll
```

| flag | 为何必须 |
| --- | --- |
| `-g` | 保留调试信息 → 告警能给出**源码行列与文件**（切片/报告都依赖它） |
| `-O0` | 不优化 → 保留 `alloca`+`load`/`store` 模式，循环守卫与下标是同一槽的多次 load，**守卫关联与符号仿射才有效** |
| `-fno-discard-value-names` | 保留 SSA 变量名 → 报告/切片里是 `%idxprom` 而非 `%12`，可读性更好 |

> macOS 上 clang16 通常在 `/opt/homebrew/opt/llvm@16/bin/clang`。

---

## 四、基本运行

```bash
# 最简：检测并打印 MUST/MAY 告警
Release-build/bin/bof foo.ll

# 同时把结构化报告写成 JSON
Release-build/bin/bof foo.ll -bof-report=report.json
```

### 终端输出示例

```
[BufferOverflowChecker] start buffer overflow analysis
[BufferOverflowChecker] MUST buffer overflow (array/pointer access)
  Base       : ValVar ID: 31
   %arrayidx = getelementptr ... { "line": 12, "col": 5, "file": "foo.c" }
  Access     : [16, 16]
  Valid range: [0, 9]  (elements)
  Location   : { "line": 12, "col": 5, "file": "foo.c" }
[BufferOverflowChecker] done, N buffer-overflow bug(s) reported
```

- **MUST**：偏移区间完全落在合法范围之外 → 必然越界。
- **MAY**：偏移区间与合法范围部分重叠（或下标退化为无界）→ 可能越界。
- `Access` 是访问偏移区间，`Valid range` 是缓冲区合法下标范围。

---

## 五、命令行选项总览

| 选项 | 默认 | 说明 |
| --- | --- | --- |
| `<input.ll/.bc>` | — | 输入 LLVM IR（可多个） |
| `-bof-report=<file>` | 空 | 把 sound 报告导出为 JSON（不写则只打印终端） |
| `-llm-config=<file>` | 空 | LLM triage 配置 JSON（见第六节） |
| `-llm-slice-out=<file>` | `bof_slices.json` | 切片导出路径（**只要有幸存 MAY 就会写**） |
| `-llm-sidecar=<py>` | 空 | Python sidecar 路径；为空即「仅导出切片」模式 |

> 选项加载顺序：`-llm-config` 文件 → 环境变量回退（`BOF_LLM_*`）→ 命令行 `-llm-slice-out`/`-llm-sidecar` 覆盖路径。

---

## 六、LLM 辅助 MAY-triage 使用

针对**循环归纳越界**（如 `for(i=0;i<=10;i++) a[i]`）——区间域建模不了循环守卫，下标退化为无界，真越界与安全访问**都报 MAY、无法区分**。本特性把这类幸存 MAY 抽成**结构化切片**，交大模型语义判定，把 MAY 分流为「疑似越界（`LLM_SUSPECT`）/ 维持 MAY」。

> **soundness 硬约束**：大模型**只能升级（MAY→`LLM_SUSPECT`）或维持 MAY，绝不降为 SAFE**。判 SAFE 仅降低展示优先级，MAY 仍保留。最坏只多报、绝不漏报。

它有三种使用模式：

### 模式 A：仅导出切片（无 API，推荐先用这个看 schema / 人工校对）

```bash
Release-build/bin/bof foo.ll -llm-slice-out=reports/bof_slices.json
```

输出：
```
[LLMTriage] exported K slice(s) to reports/bof_slices.json
[LLMTriage] no LLM endpoint configured; slices exported for manual review only.
```

这份 `bof_slices.json` 就是给大模型的输入，也可人工审阅。**不依赖网络、不改 sound 结果。**

### 模式 B：接真实大模型（OpenAI 兼容接口）

两种配置方式，二选一或组合：

**方式 1 —— 配置文件 `-llm-config`：**

```json
{
  "api_url": "https://api.openai.com/v1/chat/completions",
  "api_key": "sk-xxxx",
  "model": "gpt-4o-mini",
  "threshold": 0.7,
  "sidecar": "svf-llvm/tools/BOF/llm_triage.py",
  "slice_out": "reports/bof_slices.json",
  "verdict_out": "reports/bof_verdicts.json",
  "python": "python3"
}
```
```bash
Release-build/bin/bof foo.ll -llm-config=my_llm.json
```

**方式 2 —— 环境变量 + 命令行（API Key 不落配置文件）：**

```bash
export BOF_LLM_API_URL="https://api.openai.com/v1/chat/completions"
export BOF_LLM_API_KEY="sk-xxxx"
export BOF_LLM_MODEL="gpt-4o-mini"
Release-build/bin/bof foo.ll \
    -llm-sidecar=svf-llvm/tools/BOF/llm_triage.py \
    -llm-slice-out=reports/bof_slices.json
```

> **启用条件**：必须 `api_url` 非空 **且** `sidecar` 路径非空（即 `hasApi()` 为真）才会真正调用大模型。API Key 经环境变量传给子进程，不出现在命令行。

启用后流程：C++ 导出切片 → `std::system` 调 `python3 llm_triage.py --slices … --out … --threshold …` → sidecar 调 LLM（`temperature=0`）写 `bof_verdicts.json` → C++ 读回，对 `verdict=OUT_OF_BOUNDS && confidence≥threshold` 的 MAY 终端标注 `LLM_SUSPECT`：

```
[LLMTriage] LLM_SUSPECT (overflow, conf=0.95) at foo.c:19:9
    max_index_reasoned: 10
    rationale: loop guard i<=10 reaches index 10, capacity [0,9]
```

> 任何失败（网络/密钥/解析/超时/非零退出）一律降级为「无裁决」，所有 MAY 原样保留——不阻塞、不改变 sound 结论。

### 模式 C：离线 mock（测管道连通性，无需 API）

`tests/basic_tests/mock_sidecar.py` 把每个切片都判 `OUT_OF_BOUNDS(0.99)`，用于验证 C++↔sidecar↔标注链路：

```bash
BOF_LLM_API_URL="mock://test" BOF_LLM_MODEL="mock" \
Release-build/bin/bof foo.ll \
    -llm-sidecar=tests/basic_tests/mock_sidecar.py \
    -llm-slice-out=reports/bof_slices.json
```

> mock 是「全判越界」，只验证机制是否跑通，**不验证裁定准确性**（连安全循环也会被标 `LLM_SUSPECT`）。

### 切片 schema（`bof-slice/v1`，给大模型的输入）

```jsonc
{
  "schema": "bof-slice/v1",
  "slice_count": N,
  "slices": [{
    "id": "GEP_OOB@file:line:col",
    "static_verdict": "MAY",
    "access": {
      "file/line/col": "访问点源码位置",
      "base": "数组指针友好名",
      "index_expr": "下标的符号仿射形",
      "index_range_static": { "lower","upper","is_top","is_bottom" }  // 通常无界
    },
    "buffer": { "capacity": {...}, "is_heap": false, "domain": "elements|bytes" },
    "induction": { "var","init","step","update_op" },   // 尽力而为，非线性下标可能为空
    "guards": [ { "predicate":"<=","lhs","rhs","rhs_range","valid":true } ], // 守卫关联失败时为 []
    "code_snippet": "访问点上下文源码（含 for 头），守卫/归纳为空时的兜底"
  }]
}
```

### 裁决 schema（`bof-verdict/v1`，sidecar 回写）

```jsonc
{
  "schema": "bof-verdict/v1",
  "model": "gpt-4o-mini",
  "threshold": 0.7,
  "verdicts": [{
    "id": "GEP_OOB@file:line:col",
    "verdict": "OUT_OF_BOUNDS | SAFE | UNKNOWN",
    "confidence": 0.0~1.0,
    "max_index_reasoned": "推理出的最大下标",
    "rationale": "简短理由"
  }]
}
```

> 注意：线性下标（`a[i]`）能抽出守卫与步长；非线性/多变量下标（`a[i*i]`、`a[i*5+j]`、`a[i*j]`）的 `guards`/`induction` 可能为空，此时**全靠 `code_snippet` 让大模型推理**——这是结构化抽取的固有边界，属预期行为。

---

## 七、各 sound 特性的触发与示例

下列特性**默认全开**，无需任何选项，命中即报。回归用例在 `tests/basic_tests/`。

| 特性 | 示例越界 | 对应用例 |
| --- | --- | --- |
| 栈数组 GEP | `int a[10]; a[16]=…;` → MUST | `stack_oob.c` |
| 堆分配 | `malloc(n)`/`calloc(n,sz)`/`realloc(p,n)` 后越界 → MUST | `heap_oob.c` |
| 内存拷贝 | `char b[8]; memcpy(b,src,16);` → MUST | `memcpy_oob.c` |
| 跨函数 | `void f(int*p){ p[11]=…; } f(a);` → MUST/MAY | `interproc_oob.c` |
| 符号仿射少分配 | `buf=malloc(len-2); memcpy_s(buf,len,…,len);` → MUST | `alloc_offbyone_memcpy_s.c` |
| 复杂下标 | `a[b*c]`、`a[b*c+b]` | `complex_index.c` / `complex_heap.c` |
| 循环归纳（LLM triage 目标） | `for(i=0;i<=10;i++)a[i]` → MAY | `loop_oob.c` |

---

## 八、回归测试

```bash
cd tests/basic_tests
bash run_tests.sh
```

`run_tests.sh` 会：
1. 用 clang 把每个 `*.c` 编成 IR，跑 `bof`，对照基线核对 MUST/MAY 计数；
2. 额外跑 **LLM triage overlay** 两项断言：
   - **API 为空**：`bof_slices.json` 必被写出且 schema 字段齐全；
   - **mock sidecar**：幸存的循环 MAY 被标 `LLM_SUSPECT`。

退出码 0 表示全部通过。可用环境变量覆盖：`CLANG=`、`BOF=`、`PYTHON=`。

---

## 九、常见问题（FAQ）

**Q：报告里没有源码行列 / 切片 `code_snippet` 为空？**
A：IR 没带 `-g`，或编译后 `.c` 源文件路径已变动。重新用 `clang -g -O0 -fno-discard-value-names` 编译。

**Q：切片 `guards=[]`、`induction` 为空？**
A：下标不是循环变量本身（如 `a[i*i]`、`a[i*5+j]`），结构 token 关联不上；属预期，靠 `code_snippet` 让大模型判断。也可能是没带 `-O0`/`-fno-discard-value-names`。

**Q：配了 API 却没调用大模型？**
A：检查 `hasApi()` 条件——必须 `api_url`（config 或 `BOF_LLM_API_URL`）**和** sidecar 路径（config 的 `sidecar` 或 `-llm-sidecar`）**同时非空**。

**Q：大模型会不会把真越界判成安全、导致漏报？**
A：不会。机制上 LLM **只能升级或维持 MAY，绝不清除**；判 SAFE 仅降展示优先级。sound 报告（终端 MUST/MAY、`-bof-report` JSON）完全不受 triage 影响。

**Q：`bof_slices.json` 生成在哪？**
A：默认在**运行 `bof` 的当前目录**，文件名 `bof_slices.json`；用 `-llm-slice-out=<路径>` 指定。

**Q：找不到 `bof` 二进制？**
A：先 `./build.sh`（或 `./build_bof.sh` 增量）。产物在 `Release-build/bin/bof`。
