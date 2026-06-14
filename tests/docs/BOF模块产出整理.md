# 缓冲区溢出静态检测模块（BOF）—— 产出整理

> 基于 SVF（Static Value-Flow Analysis）框架自研的缓冲区溢出（Buffer Overflow / 数组越界）静态分析检测器。
> 作者：Yaokun Yang ｜ 语言：C++ ｜ 依赖：LLVM、SVF
> 状态：已完成核心检测能力 + 三项进阶特性（堆分配统一识别、内存拷贝函数检查、跨函数越界检测）
> + 符号仿射关系判定（攻克"分配尺寸与拷贝长度同源但数值未知"的 off-by-one 少分配漏报）。

---

## 一、项目背景

SVFmem+ 是在静态分析工具 SVF / SABER 框架之上，扩展内存安全缺陷检测能力的项目，目标是为编译后的 C 程序（`.bc` 字节码）提供 Use-After-Free（UAF）、未定义使用、数组越界等缺陷的检测支持。

**BOF 模块是我独立设计并实现的子系统**，专注于解决其中的「缓冲区/数组越界访问」检测问题。它独立于 SABER 既有实现，基于 SVFIR（程序中间表示）与区间抽象（Interval/Range Analysis）实现了对**栈对象、堆对象、跨函数指针传递**的越界访问检测，并覆盖 GEP 数组索引与 memcpy/memset/strcpy 等内存拷贝写越界两类场景。

代码分布：

| 层 | 路径 | 说明 |
| --- | --- | --- |
| 工具入口 | `svf-llvm/tools/BOF/bof.cpp` | 命令行工具 `bof`，负责构建 SVFIR 并驱动检测 |
| 头文件 | `svf/include/BOF/*.h` | 核心类声明（抽象域 / 区间分析 / 分配识别 / 拷贝规则 / 传播节点 / 检测器） |
| 实现 | `svf/lib/BOF/*.cpp` | 对应实现 |
| 构建 | `svf-llvm/tools/BOF/CMakeLists.txt`、`tools/CMakeLists.txt` | 集成进 SVF 构建体系，产出可执行文件 `bof` |
| 测试 | `tests/`（仓库根，与源码目录分离） | 回归用例集（栈/堆/跨函数/复杂索引/memcpy 越界基线 + 一键 `run_tests.sh`） |

---

## 二、整体架构

```
                  ┌──────────────────────────────┐
   bof.cpp  ─────▶│   BufferOverflowChecker       │  检测主流程（初始化 + worklist 不动点传播）
                  └──────────────┬───────────────┘
                                 │ 依赖
   ┌───────────────┬─────────────┼──────────────┬──────────────────────┐
   ▼               ▼             ▼              ▼                      ▼
┌──────────┐ ┌──────────────┐ ┌──────────────┐ ┌────────────────┐ ┌────────────────────┐
│RangeAnaly│ │AllocAPI      │ │MemCopyAPI    │ │RangeFlowNode   │ │ 过程间传播          │
│sis       │ │Registry      │ │Registry      │ │(worklist 节点) │ │ (CallPE/RetPE/SVFG)│
│变量/缓冲区│ │堆分配统一识别 │ │拷贝函数规则表 │ │根对象+字节偏移 │ │ 跨函数越界          │
└────┬─────┘ └──────┬───────┘ └──────────────┘ └────────────────┘ └────────────────────┘
     │              │
     └──────┬───────┘
            ▼
    ┌───────────────┐
    │     Range     │  区间抽象域（格代数 / 算术 / 位运算 / 比较 / widening）
    └───────────────┘
```

模块按职责清晰分层，自底向上：

1. **`Range`** —— 区间抽象域（Interval Domain），含 widening
2. **`RangeAnalysis`** —— 基于 SVFIR 的变量取值区间分析与缓冲区尺寸分析（含 Phi/Select）
3. **`HeapAllocationHandler` / `AllocAPIRegistry`** —— 堆分配 API 统一识别与分配尺寸推导
4. **`MemCopyAPIRegistry`** —— 内存拷贝/字符串函数越界规则表
5. **`RangeFlowNode`** —— 值流传播工作表节点（含跨函数调用深度）
6. **`BufferOverflowChecker`** —— 检测主流程编排、过程间传播与越界判定/报告

---

## 三、核心技术实现

### 1. `Range`：区间抽象域

实现了一套完整的**整数区间抽象域**，作为越界检测的数值计算基础：

- **格结构（Lattice）**：定义 `TOP = [-INF, INF]`、`BOTTOM`（空集），提供 `join`（并/上确界）、`meet`（交）与 **`widening`（拓宽）** 操作，保证循环场景下数据流分析收敛。
- **算术运算**：`add/sub/mul/div/mod/negate`，实现**溢出安全的 `safeAdd/safeMul`**（超 64 位边界饱和到 `±INF`）、除零退化为 `TOP`，保证可靠性（soundness）。
- **位运算 / 移位**：`bit_and/or/xor/not`、`shl/lshr/ashr`，处理负移位、过宽移位（≥63）等未定义行为边界。
- **比较与逻辑**：`eq/ne/lt/le/gt/ge`、`logical_and/or/not`、三元 `select`，均返回保守结果。
- **工具方法**：`isTop/isBottom/isConstant/isSubset`，及修正了下溢风险的安全 `toString`。

> 亮点：显式处理整数溢出、除零、移位未定义行为，并引入 widening，避免静态分析中常见的环绕误判与不收敛。

### 2. `RangeAnalysis`：变量区间与缓冲区尺寸分析

- **缓冲区尺寸分析 `analyzeBufferRange`**（重载支持栈/堆）：栈对象兼容 `int a[16]`（定长 → `getNumOfFlattenElements`）与 `int a[n]`（变长 → `getNumOfElements`）；堆对象基于 `getByteSizeOfObj` 推导字节尺寸；**边界放宽至 `size>=1`**，覆盖 `char buf[1]` 等单字节缓冲区。
- **变量区间分析 `analyzeVarRange`**：沿 SVFIR 的 Load/Store/Copy/BinaryOp **及 `PhiStmt`/`SelectStmt`** 做递归值流求解，支持 12 类二元运算；**补齐 Phi 处理后，循环归纳变量不再退化为 TOP**，是越界检测精度的关键。
- 设置**最大递归深度**防环路无限递归，引入 **memoization 缓存**先标 `TOP` 再求解，破解递归环并避免重复计算。

### 3. `AllocAPIRegistry` / `HeapAllocationHandler`：堆分配统一识别

把"是否分配函数 + 尺寸参数位置 + 尺寸计算方式"集中管理，识别来源**按优先级合并**，解决"有的 malloc 识别得到、有的识别不到"问题。系统中分两条路径互补：

- **Addr 路径（`initialize`）**：SVF 已建模的分配（LHS 指向 `HeapObjVar`，由 `extapi.json` 的 `ALLOC_HEAP_RET` 注解自动建模）直接走 `Addr` 语句捕获，用 `getByteSizeOfObj()` 取字节尺寸；
- **Call 路径（`AllocAPIRegistry::resolveAlloc`）**：对调用点按以下优先级识别——
  1. **BOF 本地表优先**：本地表给出最精确的尺寸规格（含 `calloc`/`realloc`/平台私有分配函数的正确参数下标），命中即返回；
  2. **复用 `SaberCheckerAPI::getCheckerAPI()->isMemAlloc()`** 大表兜底识别；
  3. **复用 `ExtAPI`**：`is_alloc / is_realloc / hasExtFuncAnnotation(F,"ALLOC_HEAP_RET")` 兜底识别，识别后按是否 `realloc` 决定尺寸参数取 arg1 还是 arg0。

> 本地表自包含在 BOF 内（覆盖 `malloc/xmalloc/valloc/alloca/strdup/strndup`、`calloc/SoftBusCalloc/SysCalloc`、`realloc/LOS_MemAlloc/LOS_MemRealloc`、`VOS_MemAlloc/_TIFFmalloc/kmem_cache_alloc` 等），**不改 `extapi.json`/`SaberCheckerAPI`**，避免对 SVF master 的侵入式改动。

尺寸计算改为**数据驱动**（`AllocSpec{kind, sizeArgIdx, countArgIdx}`）：区分 malloc 型（`size=arg[sizeArgIdx]`）与 calloc 型（`size=arg[countArgIdx]×arg[sizeArgIdx]`），并**按 API 配置修正 realloc 系列的尺寸参数下标**（`realloc` 取 arg1、`LOS_MemRealloc` 取 arg2），不再恒取 arg0。尺寸从**实际调用点参数**（`CallICFGNode::getArgument`）读取，而非被调函数形参。

### 4. `MemCopyAPIRegistry`：内存拷贝/字符串函数越界检查

对标 AE 的 `extAPIBufOverflowCheckRules`，以**规则表 `funcName → {bufArgIdx, lenArgIdx}`** 驱动检查：

- 覆盖 `memcpy/memmove/memset/strncpy/__memcpy_chk/bcopy` 等，dst 写越界与 src 读越界均检查；
- 校验逻辑：`访问区间 = [baseOffset.lower, baseOffset.upper + len.upper - 1]`，若不落入 `getBufferRange(base)` 则报越界；
- `strcpy/strcat` 无显式 len，由 `strlen(src)` 推导长度（未知时保守按 may 处理）；
- 统一到**字节偏移模型**，用 `elemSize` 做字节↔元素换算，使堆/栈缓冲区一致比较。

### 5. `RangeFlowNode` + 跨函数传播

- 工作表节点封装：`base`（当前变量）、`parent`（缓冲区根对象，**按对象级聚合**）、`accumulate_offset`（统一为**字节偏移区间**）、`isHeap`、`callDepth`（跨函数展开深度上限）、`callContext`（**k=1 调用点上下文**）。
- **过程间传播**：在 worklist 中新增 `CallPE`（实参→形参）与 `RetPE`（return→调用点返回值）边处理，把"缓冲区根 + 累积偏移"随参数/返回值流动，打通 `f(int* arr){ arr[i]=...; }` 这类跨函数越界；配合**调用深度上限 + widening + "状态变化才入队"** 防发散、防误报。
- **k=1 上下文敏感（call-site 敏感）升级**：在传播前做一遍 `bindCallArguments` 预处理，把每个直接调用点的**标量整型实参区间**绑定到 `(调用点, 形参)`；当形参在被调函数内用作索引（含 `b*c`、`b*c+b` 等由多个形参派生的复合表达式）时，`analyzeVarRange` 沿绑定精确恢复其区间，而非跨 `Call` 边退化为 `TOP`。由此 `write_at(a, 11)` / `a[b*c]` 这类常量/可计算索引被判定为 **MUST**，且不同调用点互不污染。
  - **按上下文的不动点 key**：`flowState` 的去重键由 `(parent, base)` 扩展为 `(parent, base, callContext)`。修复了"同一缓冲区从**同一函数内的多个调用点**传入同一被调函数时，因偏移相同被'状态未变化'守卫吞掉、只有首个调用点得以传播"的缺陷；上下文集合有限（k=1 调用点 × 受限调用深度），终止性仍有保证。
  - **按上下文的报告去重**：`reportBufferOverflowError` 的去重键由 `(源码位置, 类型)` 扩展为 `(源码位置, 类型, callContext)`，使**同一被调指令**在不同调用上下文下的越界各自上报（如 `write_at(a,11)` 与 `write_at(a,10)` 都落在同一条 `p[idx]`），而非被首个抑制。

### 6. `BufferOverflowChecker`：检测主流程

采用 **初始化 + worklist 不动点传播** 的两阶段算法：

- **初始化**：遍历 `Addr` 识别栈/堆对象、遍历 `Call` 经 `AllocAPIRegistry` 识别分配点，**合并两条路径去重**，计算缓冲区尺寸入 worklist。
- **传播**：从工作表取节点，沿出边处理 GEP/Copy/Store/Load **及 Call/Ret**：
  - GEP 偏移**逐维累加**（修正了原堆分支覆盖式赋值丢失累积偏移的缺陷），区分按字节（堆）与按元素（栈，结合 `getFlattenedElemIdx` 处理结构体扁平化下标）；
  - 引入**不动点收敛**："状态变化才入队" + widening，保证循环场景终止与精度。
- **报告**：接入统一的 `SVFBugReport`，输出**带源码行号/文件名**、**按（位置 + 类型 + 调用上下文）去重**、并区分**必然越界（must）/ 可能越界（may）** 级别与错误来源（`GEP_OOB/MEMCPY_OOB/MEMSET_OOB/STRCPY_OOB`）。

### 7. 符号仿射关系判定：攻克"同源但数值未知"的 off-by-one 少分配

区间域是**非关系型**的：当拷贝/写入长度 `len` 来自反序列化、其数值被保守提升为 `TOP` 时，区间域丢失了"分配尺寸与拷贝长度同源"这一关键关系，无法证明 `malloc(len-1)` + `memcpy_s(buf, len, .., len)` 这类少分配越界。为此引入**轻量符号仿射归一化**，作为 `checkMemoryOps` 在 `len.isTop()` 跳过**之前**的补救分支：

- **仿射归一化 `RangeAnalysis::analyzeAffine(var)`**：把一个标量整型表达式化为一组 `AffineTerm{base, offset}`（`base` 为符号基的结构化标识串，空串表示常量）。沿 def-use 边遍历 `Copy/Load/Store/Gep/BinaryOp(Add/Sub 常量)/Phi/Select`，把 `len`、`len-1`、`phi(len, len-1)`、`select(_, len, len-1)` 归一化为「同一基 + 整数偏移区间」（带 memoization 与深度上限防环）。
- **结构化位置标识 `RangeAnalysis::locationToken(var)`**：把指向同一源码位置的 SSA 值（如 `-O0` 下对同一结构体字段的两次独立 load）规范化为相同 token，从而**无需指向分析**即可跨指令匹配"分配结果"与"后续拷贝缓冲区"。
- **分配尺寸符号记录**：`initialize` 用 `allocResultSym`/`allocAddrSym` 旁路表，在数值尺寸为 `TOP` 时**仍记录**堆缓冲区的分配尺寸符号（`HeapAllocationHandler::getAllocSizeOperand`）。除 SVF 已建模的 `malloc`（`AddrStmt→HeapObjVar`）外，**额外增设一趟 `getCallSiteSet()` 遍历**，用 `getRetICFGNode()->getActualRet()` 捕获 SVF 核心**未建模为堆对象**的平台分配器（如 `OsalMemCalloc`，无 `AddrStmt`/`CallPE`）的结果符号。
- **判定（base 相消）**：把分配尺寸表达式与拷贝长度表达式都归一化后，当两侧 base **指针严格相等**时，base 在比较中相消，仅凭偏移即可证明 `分配 < 拷贝`——`TOP` 不再是障碍。**MUST/MAY 分级**：恒定少分配（偏移恒负）→ MUST；条件少分配（偏移区间含 0，如 `[-1,0]`）→ MAY，与"复杂分支条件触发"语义一致。
- **零新增误报保证**：仅当两侧 base 指针严格相等时才触发，不做任何别名推断；等长拷贝（偏移均为 0）判定为不越界，不误报。

> 涉及文件均在 BOF 内：`RangeAnalysis.{h,cpp}`（`analyzeAffine`/`locationToken`）、`HeapAllocationHandler.{h,cpp}`（`getAllocSizeOperand`）、`BufferOverflowChecker.{h,cpp}`（旁路表 + `recordAllocSizeSym`/`findAllocSizeForBuffer`/`trySymbolicUnderAlloc`）。未触碰 SVF 核心/master。

### 8. `LLMTriage`：LLM 辅助 MAY-triage（循环归纳越界分流）

区间域无法建模循环守卫 `i <= 10`（`analyzeVarRange` 不处理 `CmpStmt/BranchStmt`），归纳变量经 widening 抬到 `TOP` → 循环访问只能报 **MAY**（保守、噪声大）。本特性在 sound 区间分析之上叠加**纯 overlay**：对幸存 MAY 中「`GEP_OOB` 且下标退化为 `TOP`」的循环访问，抽取**最小完备代码切片**交大模型语义判定，把 MAY 进一步分流为「疑似越界（`LLM_SUSPECT`）/ 维持 MAY」。

- **切片抽取（只读 IR）`LLMTriage::collectSlice`**：复用既有基建产出 `bof-slice/v1` JSON——`access`（位置/base/`analyzeAffine` 索引符号式/索引静态区间）、`buffer`（`getBufferRange` 容量/堆栈域）、`induction`（扫 `BinaryOPStmt(Add/Sub)` 经 `locationToken` 关联归纳槽取步长）、`guards[]`（扫 `CmpStmt`，操作数 `locationToken` 与下标 token 相等即关联谓词与界）、`code_snippet`（访问点上下文源码窗口）。
- **C++/Python 解耦**：C++ 仅导出切片与读回裁决，不内嵌 HTTP；实际调用隔离到 Python sidecar `llm_triage.py`（标准库 `urllib`，OpenAI 兼容，`temperature=0` 利复现）；API Key 经环境变量传子进程（不落命令行）。
- **soundness 硬约束**：LLM **永不**把 MAY 清成 SAFE，只允许「升级为 `LLM_SUSPECT` 标注」或「维持 MAY」；sound 报告（`SVFBugReport`/`-bof-report`）完全不变，triage 结果仅出现在终端标注与 `bof_slices.json`/`bof_verdicts.json` 两个 overlay 文件。最坏只多报、绝不漏报。
- **API 为空兜底**：无论是否配置端点，始终先写 `bof_slices.json`，供人工校对与查看 schema；任何 sidecar 失败一律按「无裁决」保持 MAY。
- 命令行：`-llm-config`/`-llm-slice-out`/`-llm-sidecar`，端点回退环境变量 `BOF_LLM_API_URL/KEY/MODEL`。

> 涉及文件：`svf/include/BOF/LLMTriage.h` + `svf/lib/BOF/LLMTriage.cpp`（新增）、`svf-llvm/tools/BOF/llm_triage.py`（新增）、`BufferOverflowChecker.{h,cpp}`（`PendingReport.indexVar` 透传 + `flushReports` overlay 尾段）、`bof.cpp`（CLI）。回归用例 `tests/basic_tests/loop_oob.c` + `mock_sidecar.py`。

---

## 四、技术亮点小结

- **完整的抽象解释（Abstract Interpretation）实践**：自研区间抽象域 + widening + worklist 不动点传播，覆盖算术、位运算、移位、逻辑、比较等全套语义，并正确处理 Phi/Select 使循环归纳变量保持精度。
- **可靠性优先（Sound）**：溢出饱和、除零退化、移位未定义行为、递归深度限制、缓存破环、不动点收敛，系统性规避静态分析常见陷阱。
- **堆栈统一字节偏移模型**：同一套传播框架处理栈数组、堆缓冲区，并正确区分「按元素」与「按字节」语义。
- **数据驱动的可扩展规则表**：`AllocAPIRegistry`（分配识别）与 `MemCopyAPIRegistry`（拷贝越界）以配置表方式扩展，复用 SVF 既有 `SaberCheckerAPI`/`ExtAPI` 注解，避免重复造轮子。
- **过程间分析（k=1 上下文敏感）**：worklist 补 `CallPE/RetPE` 实现跨函数越界检测，并通过调用点级（call-site）形参区间绑定，使跨函数透传的常量/派生索引（如 `a[b*c]`）精确判定为 must；`flowState` 与报告去重均按调用上下文区分，多调用点互不污染，并预留向 SVFG 全上下文敏感分析演进的路径。
- **符号仿射关系判定（轻量 symbolic interval）**：在非关系型区间域之上叠加"同一符号基 + 整数偏移"的轻量关系判定，当分配尺寸与拷贝长度同源（共享同一 base）时 base 相消，即便两侧数值为 `TOP` 也能证明 off-by-one 少分配越界；并打通 SVF 未建模平台分配器（`OsalMemCalloc`）的尺寸符号捕获。严格要求 base 指针相等才触发，零新增误报。
- **良好工程化**：职责分层清晰、Doxygen 注释规范、结构化报告（`SVFBugReport`，含源码位置 + must/may 分级），完整集成进 SVF CMake 构建与回归测试体系，产出独立命令行工具 `bof`。

---

## 五、回归验证与已知限制

### 编译与运行

- 经 `setup.sh` + `build.sh`（LLVM 16 + Z3）**全量编译通过**，产出独立可执行文件 `bin/bof`；日常改完 BOF 源码后用 `./build_bof.sh`（增量，仅重建 `bof` 目标，几秒到几十秒）。
- 支持 `-bof-report <file>` 选项将结构化结果 dump 为 JSON（`SVFBugReport::dumpToJsonFile`）。

### 回归用例（`tests/basic_tests/`，一键 `./run_tests.sh`，当前 9/9 全绿）

| 用例 | 场景 | 期望 | 实测结果 |
| --- | --- | --- | --- |
| `stack_oob.c` | 栈数组写/读越界 | `a[10]`、`a[16]` on `int a[10]` 报 MUST | ✅ 两处 MUST；循环 `a[i]` 报 MAY（见限制 1） |
| `single_byte.c` | 单字节缓冲区 | `b[1]` on `char b[1]` 报 MUST，`b[0]` 不报 | ✅ 1 MUST，`size>=1` 边界生效 |
| `heap_oob.c` | 堆越界（malloc/calloc/realloc） | 三处 MUST，`heap_safe` 不报 | ✅ 3 MUST（calloc 取 `n*sz`、realloc 取 arg1 尺寸均正确），安全用例不误报 |
| `interproc_oob.c` | 跨函数指针透传（k=1 上下文） | 常量索引随实参流入被调函数精确判定 | ✅ 2 MUST（`write_at(a,11)`、`write_at(a,10)` 各自报告，`(a,9)` 安全） |
| `complex_index.c` | 复杂索引：`a[]`+标量 `b,c` 传参，函数内 `a[b*c]`/`a[b*c+b]` | 派生索引经 k=1 形参绑定精确恢复 | ✅ 3 MUST（`g[12]`、`g[10]`、`m[20]`），`g[9]`/`m[15]` 安全 |
| `complex_heap.c` | 堆缓冲区 + 计算索引 `a[b*c]`（字节域换算） | `malloc/calloc` 堆缓冲区计算索引越界 | ✅ 2 MUST（`p[16]`=字节 64、`q[9]`=字节 36），`p[9]`/`q[6]` 安全 |
| `memcpy_oob.c` | memcpy/memset/strncpy 越界 | 三处 MUST，`copy_safe` 不报 | ✅ 3 MUST，安全用例不误报 |
| `struct_oob.c` | 结构体字段索引越界 | `data[8]` on `char data[8]` 报 MUST | ✅ 1 MUST，GEP 字段扁平化下标越界生效 |
| `alloc_offbyone_memcpy_s.c` | 符号少分配（仿射关系判定） | 同源但数值 TOP 时证明 `分配<拷贝` | ✅ 1 MUST（`const_under`=`malloc(len-2)` 恒定少分配）+ 1 MAY（`cond_under`=分支上 `malloc(len-1)` 条件少分配），`exact_alloc`（等长）不报 |

> 测试目录已从 `svf-llvm/tools/BOF/tests/` 迁出至仓库根 `tests/`，与 `svf/`、`svf-llvm/` 源码分离，实现**测试与开发解耦**：`tests/basic_tests/`（上表 9 个最小回归用例，一键 `run_tests.sh` 与 MUST/MAY 基线严格比对）+ `tests/advanced_tests/`（真实项目链接 bc 困难用例，`run_hard_tests.sh` 汇总命中）。`run_tests.sh` 自动定位 `clang`（优先 `llvm@16`）与 `bof`，逐例编译为 LLVM IR、运行检测器并与基线比对，全绿退出码为 0。

### 困难用例验证（`tests/advanced_tests/`，真实项目链接 bc）

在 50 缺陷的内核 / ubs_engine 用例上 BOF **100% 命中全部 10 个注入 BO 缺陷**；OpenHarmony WLAN 困难用例经本次**符号仿射关系判定**改进后，BO 召回从 **4/8 提升到 6/8（25% → 75%）**——新增命中的 BO-4（`rxEapol->buf = malloc(len-1)` + `memcpy_s`）与 BO-5（`dst->beaconIe = OsalMemCalloc(len-1)` + `memcpy_s`）正是"分配尺寸与拷贝长度同源但数值未知"的典型 off-by-one 少分配。简单版回归 9/9 不退化、其它困难用例计数不变，**零新增误报**。详见 `tests/advanced_tests/ANALYSIS.md`。

### 已知限制（保守近似，均偏向 sound、不漏报）

1. **循环归纳变量**：`for(i...) a[i]` 中 `i` 经 Phi 在不动点+widening 后通常被拓宽为 `TOP`，导致循环内数组访问报 **MAY** 而非精确判定（过近似，不漏报）。**可选**通过特性 8 的 **LLM 辅助 MAY-triage** overlay 进一步分流（抽取切片交大模型判定为 `LLM_SUSPECT` 或维持 MAY），该层恪守"绝不降为 SAFE"的 soundness 约束，不改 sound 报告。
2. **跨函数 k=1 上下文敏感（call-site 敏感）**：缓冲区根 + 累积偏移随 `CallPE/RetPE` 透传，并通过 `bindCallArguments` 把**标量整型实参**绑定到 `(调用点, 形参)`；当索引由形参（含 `b*c` 等派生表达式）构成时可精确恢复为 **MUST**，且 `flowState`/报告均按调用上下文区分，多调用点互不污染。当前为 **k=1（单层调用点）敏感**：①**多层调用链**中第二层及更深的标量实参（其本身是上一层形参）仍退化为 `TOP`；②调用深度上限 `MAX_CALL_DEPTH=4` 防发散。这两类情形仍偏保守（报 MAY 或不报），不漏报。
3. **栈非 char 数组的字节换算**：memcpy 类检查统一到字节域，栈缓冲区按 `totalBytes/numUnits` 推元素字节数做换算，对非 char 数组为近似。
4. **指针存储槽不作缓冲区**：`char* p;` 这类指针 alloca 不作为可索引缓冲区 seed（其指向的缓冲区经 alloc 路径 + Store/Load 链跟踪），避免泄漏 `[0,0]` 区间造成假阳性。
5. **未知拷贝长度/源串长度**：显式 len 为 `TOP` 或 strcpy 源串未知时，**若拷贝长度与分配尺寸同源**（共享同一符号基）由符号仿射判定补救（见技术实现第 7 节）；否则仍**保守跳过**，避免误报泛滥。
6. **带乘法因子的少分配 / 字段级越界**：`calloc(num-1, IFNAMSIZ)` 循环按 `num` 写入（含乘法因子与循环边界），以及越过结构体定长数组字段但仍落在整对象内的字段级越界，超出当前轻量仿射（仅支持加减常量）与整对象字节建模的范围，保守不报（需带正因子的乘法比较 / 字段敏感，有 FP 风险，本次未纳入）。

> 上述限制对应设计文档中的固有取舍，可通过后续迁移到 SVFG（全上下文/字段敏感）做更高 k 值的上下文敏感分析进一步提升精度。

---

## 六、简历可用描述（可直接复制）

### 版本 A：项目经历段落（详细）

> **基于 SVF/LLVM 的缓冲区溢出静态检测器（C++）** — 个人独立模块
>
> 在静态分析框架 SVF 之上独立设计并实现缓冲区溢出（数组越界）检测子系统 BOF，支持对编译后 C 程序（LLVM bitcode）的栈数组、堆缓冲区与跨函数指针传递进行越界访问检测。
> - 基于**抽象解释**自研整数**区间抽象域**，实现 join/meet/widening 格运算及算术、位运算、移位、逻辑、比较等完整语义，并通过溢出饱和、除零退化保证分析可靠性（soundness）。
> - 基于 SVFIR 实现**变量区间分析**（含 Phi/Select），采用 **worklist 不动点 + widening** 的传播算法统一处理栈/堆对象，正确区分「按元素」与「按字节」偏移并处理结构体扁平化下标。
> - 以**数据驱动规则表**统一堆分配 API 识别（复用 SaberCheckerAPI/ExtAPI 注解）与 **memcpy/memset/strcpy 内存拷贝越界检查**；在 worklist 中补 CallPE/RetPE 实现**跨函数越界检测**。
> - 接入统一 `SVFBugReport` 输出带源码位置、去重、must/may 分级的结构化报告，并集成进 SVF CMake 构建与回归测试体系，产出独立工具 `bof`。

### 版本 B：要点式（精简）

> - 在 SVF/LLVM 静态分析框架上独立实现缓冲区溢出（数组越界）检测器（C++）。
> - 自研整数区间抽象域（含 widening）+ worklist 不动点值流传播，统一检测栈/堆/跨函数缓冲区越界。
> - 数据驱动规则表统一堆分配识别与 memcpy/memset/strcpy 拷贝越界检查；输出带源码位置、must/may 分级的结构化报告并集成构建/测试体系。

### 关键词（技能标签）

`静态程序分析` · `抽象解释` · `区间分析（Interval/Range Analysis）` · `数据流分析 / 不动点 / widening` · `过程间分析` · `LLVM IR` · `SVF / SVFIR` · `缓冲区溢出检测` · `C++` · `CMake`

---

## 七、简历最多 4 行写法（投递用）

> 简历空间有限时，建议突出「框架 + 方法论 + 覆盖范围 + 工程落地」四个信息密度最高的点。以下任选一版，每版正好 ≤4 行。

### 推荐版（4 行，信息最全）

```
基于 SVF/LLVM 的缓冲区溢出（数组越界）静态检测器（C++，个人独立模块）：
· 自研整数区间抽象域（join/meet/widening）+ worklist 不动点值流传播，覆盖算术/位运算/移位/Phi 等完整语义；
· 统一栈/堆/跨函数缓冲区越界检测，数据驱动规则表支持堆分配识别与 memcpy/memset/strcpy 拷贝越界检查；
· 接入 SVFBugReport 输出带源码位置、must/may 分级的结构化报告，集成 CMake 构建与回归测试，产出独立工具 bof。
```

### 紧凑版（3 行，留白更多）

```
缓冲区溢出静态检测器（基于 SVF/LLVM，C++，独立设计实现）：自研整数区间抽象域 + worklist 不动点传播，
统一检测栈/堆/跨函数数组越界，并支持 memcpy/memset/strcpy 拷贝越界；数据驱动规则表复用 SaberCheckerAPI/ExtAPI，
输出带源码位置与 must/may 分级的结构化报告，集成进 SVF 构建与回归测试体系。
```

### 一句话版（1 行，用于技能/项目列表条目）

```
缓冲区溢出静态检测器（SVF/LLVM，C++）：基于抽象解释的区间分析 + 跨函数值流传播，检测栈/堆/拷贝函数数组越界。
```

> 投递提示：
> - 若岗位偏 **安全研究 / 漏洞挖掘**，把"缓冲区溢出 / 数组越界检测""must/may 分级告警"前置；
> - 若岗位偏 **编译器 / 程序分析**，把"抽象解释""区间抽象域 + widening""过程间数据流分析"前置；
> - 有条件再补一条**量化指标**（如：覆盖 N 类内存拷贝 API、跑通 M 个回归用例 / 真实项目检出 K 处越界），会显著加分。
