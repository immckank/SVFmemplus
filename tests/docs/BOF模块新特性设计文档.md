# BOF 模块新特性设计文档

> 目标特性：
> 1. 完善堆分配函数解析（让所有堆分配 API 都能被识别为堆对象 / 推导尺寸）
> 2. 支持内存拷贝类函数（memcpy / memset / strcpy / strncpy …）的越界检查
> 3. 支持跨函数（interprocedural）数组越界检测
> 4. 符号仿射关系判定（攻克"分配尺寸与拷贝长度同源但数值未知"的 off-by-one 少分配漏报）
>
> 参考实现：`svf/lib/SABER/*`（源-汇 + SVFG 跨函数）、`svf/lib/AE/Svfexe/{AEDetector,AbsExtAPI}.cpp`（越界规则表 + memcpy/memset 建模）、`svf/lib/Util/ExtAPI.cpp`（外部函数注解）。

> **落地状态总览（2026-06 实现完成）**：四项特性**均已实现并编译通过、回归用例验证**。跨函数采用**方案 A**（worklist 补 `CallPE/RetPE` + 调用深度上限 + widening），方案 B（SVFG）作为后续演进保留未实现。特性 4 在非关系型区间域之上叠加轻量符号仿射（symbolic interval）判定。全部改动自包含在 BOF 模块内，未修改 `extapi.json`/`SaberCheckerAPI` 等 master 文件。下文各特性末尾以 **`[实现状态]`** 标注落地要点与差异。

---

## 背景：现状与差距

| 能力 | 现状 | 差距 |
| --- | --- | --- |
| 堆分配识别 | `Addr` 语句靠 SVF 的 `HeapObjVar`（ExtAPI `ALLOC_HEAP_RET` 注解自动建模）+ 自维护 `allocApiMap`（`Call` 语句） | 未注解的自定义分配函数拿不到 `HeapObjVar`；两套口径不一致；realloc 尺寸参数取错 |
| 内存拷贝检查 | 完全缺失 | `memcpy/memset/strcpy` 等写越界无法发现 |
| 跨函数 | `propagate` 只走 `Gep/Copy/Store/Load`，不处理 `CallPE/RetPE` | 指针传入子函数后越界全部漏报 |

---

## 特性 1：完善堆分配函数解析

### 1.1 问题根因
SVF 通过 `extapi.json` 的 `ALLOC_HEAP_RET` 注解，在建 SVFIR 时把分配函数的返回值自动建模为 `HeapObjVar`。**只有被注解的函数**才会在 `initialize` 的 `Addr` 分支被捕获；未注解的自定义分配函数（项目里大量平台/私有 alloc）走不到，只能靠手写 `allocApiMap` 的 `Call` 分支兜底，导致"有的识别得到、有的识别不到"。

### 1.2 设计：统一的 `AllocAPIRegistry`
新增 `svf/include/BOF/AllocAPIRegistry.h`，把"是否分配函数 + 尺寸参数位置 + 尺寸计算方式"集中管理，识别来源按优先级合并：

```
判定一个 call 是否分配点（按优先级）：
  ① SVF 已建模：LHS 指向 HeapObjVar          → 直接用 getByteSizeOfObj()
  ② SaberCheckerAPI::isMemAlloc(callee)        → 复用 SABER 的大表（CK_ALLOC）
  ③ ExtAPI: hasExtFuncAnnotation(F,"ALLOC_HEAP_RET"/"ALLOC_HEAP_ARG")
                                               → 用 get_alloc_arg_pos(F) 拿尺寸参数下标
  ④ 本地补充表 allocApiMap（保留，作为前三者的兜底/覆盖）
```

尺寸计算改为**数据驱动**，替换 `analyzeAllocSize` 里硬编码 `getArg(0)`：

```cpp
struct AllocSpec {
    enum Kind { SIZE_ARG, ELEM_MUL_ARG } kind; // malloc 型 / calloc 型
    int  sizeArgIdx;      // malloc=0, realloc=1, LOS_MemRealloc=2
    int  countArgIdx;     // calloc 的元素个数参数（ELEM_MUL_ARG 时有效）
};
// calloc:  size = arg[count] * arg[size]
// realloc: size = arg[sizeArgIdx]   （sizeArgIdx 由表/注解给出，而非恒为 0）
```

### 1.3 改造点
- `HeapAllocationHandler::isAllocAPI` → 改为查 `AllocAPIRegistry`，优先复用 `SaberCheckerAPI::getCheckerAPI()->isMemAlloc(fun)` 与 `ExtAPI`。
- `HeapAllocationHandler::analyzeAllocSize` → 按 `AllocSpec` 取尺寸参数，修正 realloc 系列。
- `BufferOverflowChecker::initialize` → 合并 `Addr`(HeapObjVar) 与 `Call` 两条路径，避免重复入队（同一分配点只产生一个根节点）。

### 1.4 验收
- 自定义 `myalloc(size)`（在 extapi.json 注解 `ALLOC_HEAP_RET ALLOC_HEAP_ARG0`）可被识别并推导尺寸。
- `realloc(p, n)` / `LOS_MemRealloc(pool, p, n)` 尺寸取自正确参数。
- `calloc(n, sz)` 尺寸 = `n*sz`（已支持，纳入统一表）。

### 1.5 `[实现状态]` ✅ 已实现
- 新增 `svf/include/BOF/AllocAPIRegistry.{h,cpp}`：`AllocSpec{kind,sizeArgIdx,countArgIdx}` + `resolveAlloc/isAllocAPI`。
- **优先级实现差异**：实际为「**BOF 本地表优先**（给出最精确尺寸规格）→ `SaberCheckerAPI::isMemAlloc` → `ExtAPI`(`is_alloc/is_realloc/hasExtFuncAnnotation`)」；SVF 已建模的 `HeapObjVar` 仍由 `initialize` 的 `Addr` 路径独立捕获（用 `getByteSizeOfObj()`）。本地表先行可保证 calloc/realloc 等取到正确参数下标，不依赖注解完整性。
- `HeapAllocationHandler::analyzeAllocSize` 从**实际调用点** `CallICFGNode::getArgument(idx)` 读尺寸，ELEM_MUL_ARG 走 `Range::mul`。
- 回归用例 `heap_oob.c`：malloc/calloc(`n*sz`)/realloc(arg1) 三处越界均正确检出，`heap_safe` 不误报。

---

## 特性 2：内存拷贝类函数越界检查

### 2.1 设计思路（对标 AE 的 `BufOverflowDetector`）
内存拷贝函数的越界本质是：**写入/读取长度 len 是否超过目标/源缓冲区可用空间**。无需逐元素模拟，只需：

```
对 call C = f(args...):
    若 f ∈ memCopyRules:
        对每条规则 {bufArgIdx, lenArgIdx}:
            buffer  = args[bufArgIdx]            // 指向某缓冲区根对象
            len     = RangeAnalysis.analyzeVarRange(args[lenArgIdx])   // 拷贝长度区间(字节)
            base, baseOffset = 解析 buffer 的根缓冲区与已累积偏移
            avail   = RangeAnalysis.getBufferRange(base)  // 缓冲区尺寸
            // 访问区间 = [baseOffset.lower, baseOffset.upper + len.upper - 1]
            if (访问区间 ⊄ avail) reportBufferOverflowError(...)
```

### 2.2 规则表（对标 `extAPIBufOverflowCheckRules`）
新增 `svf/include/BOF/MemCopyAPIRegistry.h`，结构 `funcName → vector<{bufArgIdx, lenArgIdx}>`：

```cpp
// {缓冲区参数下标, 长度参数下标}，一个函数可有多个被检查缓冲区
{"memcpy",      {{0,2},{1,2}}},   // dst=arg0, src=arg1, len=arg2（dst写 & src读都查）
{"memmove",     {{0,2},{1,2}}},
{"memset",      {{0,2}}},         // dst=arg0, len=arg2
{"strncpy",     {{0,2},{1,2}}},
{"__memcpy_chk",{{0,2},{1,2}}},
{"bcopy",       {{0,2},{1,2}}},
// strcpy/strcat 长度来自 strlen(src)，单独处理（见 2.3）
```

### 2.3 字符串函数（长度需推导）
`strcpy/strcat` 没有显式 len 参数，长度需由源串推导（对标 AE 的 `getStrlen` + `detectStrcpy/detectStrcat`）：
- `strcpy(dst,src)`：len = `strlen(src)+1`（含 `\0`）；
- `strcat(dst,src)`：len = `strlen(dst)+strlen(src)+1`；
- 初版可保守：源串长度未知时取 `TOP`，按 may-overflow 处理或跳过，避免误报泛滥。

### 2.4 元素大小换算
拷贝长度通常以**字节**为单位，而栈缓冲区尺寸以**元素个数**记。需按 AE `handleMemcpy` 的方式用 `elemSize`（`getByteSize`）做字节↔元素换算，或统一在"字节域"比较（堆对象本就是字节域，栈对象乘以元素字节数）。建议借此把堆/栈统一到**字节偏移模型**。

### 2.5 改造点
- `BufferOverflowChecker::initialize` 或新增 `checkMemoryOps(pag)`：遍历 `SVFStmt::Call`，对命中 `MemCopyAPIRegistry` 的调用执行 2.1 检查。
- 复用现有 `RangeAnalysis` 求 len、`getBufferRange` 求容量、`reportBufferOverflowError` 报告（建议新增 `errorKind` 字段区分 GEP 越界 / memcpy 越界）。

### 2.6 验收
- `char b[8]; memcpy(b, src, 16);` → 报越界（写 16 > 8）。
- `memset(b, 0, 100);` 越界报；`memset(b, 0, 8);` 不报。
- `strncpy(b, s, 4);` 在 b 容量足够时不报。

### 2.7 `[实现状态]` ✅ 已实现
- 新增 `svf/include/BOF/MemCopyAPIRegistry.{h,cpp}`：规则表覆盖 `memcpy/memmove/__memcpy_chk/__memmove_chk/wmemcpy/wmemmove`（dst写+src读双规则）、`memset/__memset_chk/wmemset/bzero`、`strncpy/strncat/__strncpy_chk/__strncat_chk`、`snprintf/vsnprintf/__snprintf_chk`、`bcopy`，以及 `strcpy/strcat/stpcpy/__strcpy_chk/__strcat_chk`（隐式长度）。
- 新增 `BufferOverflowChecker::checkMemoryOps`：遍历 ICFG 的 `CallICFGNode`，命中规则后经 `getByteBuffer` 取缓冲区根的**字节容量与字节偏移**，访问区间 `[byteOffset, byteOffset+len-1]` 与 `[0,capacity-1]` 比较，分级 must/may 报告。
- **元素↔字节换算**：堆缓冲区本为字节域；栈缓冲区按 `totalBytes/numUnits` 推元素字节数换算到字节域统一比较。
- **保守策略**：显式 len 为 `TOP`/≤0、或 strcpy 源串长度未知时跳过，避免误报泛滥。
- 回归用例 `memcpy_oob.c`：memcpy/memset/strncpy 三处越界 MUST，`copy_safe` 不误报。

---

## 特性 3：跨函数数组越界检测

### 3.1 现状限制
`propagate` 仅沿 `base->getOutEdges()` 处理 `Gep/Copy/Store/Load`，**不跟随过程间边**：
- 实参→形参：`CallPE`（call 边）
- 返回值：`RetPE`（ret 边）
因此 `f(int* arr){ arr[i]=...; }` 这类被调用函数内的访问，拿不到调用方缓冲区的尺寸与偏移信息。

### 3.2 方案 A（短期，低成本）：在 worklist 中补过程间边
保持现有上下文不敏感（context-insensitive）的 worklist 框架，新增两类边处理，把"缓冲区根 + 累积偏移"随参数/返回值流动：

```cpp
// 在 propagate() 的出边 dispatch 中新增：
else if (auto* callPE = dyn_cast<CallPE>(svfStmt)) {
    // 实参 → 被调函数形参：偏移与根对象原样传递
    SVFVar* formalParm = callPE->getLHSVar();
    worklist.push(RangeFlowNode(formalParm, srcNode.parent,
                                srcNode.accumulate_offset, srcNode.isHeap));
}
else if (auto* retPE = dyn_cast<RetPE>(svfStmt)) {
    // 被调函数 return → 调用点返回值
    SVFVar* callRet = retPE->getLHSVar();
    worklist.push(RangeFlowNode(callRet, srcNode.parent,
                                srcNode.accumulate_offset, srcNode.isHeap));
}
```

- **优点**：改动小，复用现有框架，能覆盖大多数"指针透传"场景。
- **缺点**：上下文不敏感 → 不同调用点的偏移会 join 在一起，可能误报；递归/多次调用需配合 widening 与"状态变化才入队"防发散。
- **配套**：`RangeFlowNode` 增加 `CallStack`/深度上限字段，限制过程间展开层数（参考 `ContextCond::setMaxCxtLen` 思想）。

> **`[实现状态]` ✅ 已实现（方案 A）**
> - `propagate` dispatch 用 `getEdgeKind()==SVFStmt::Call/Ret` **精确过滤**，规避 `ThreadFork/ThreadJoin`（同样匹配 `CallPE/RetPE::classof`）被误判为过程间边。
> - `handleCall`：实参→形参（`getLHSVar`），`callDepth+1`；`handleRet`：formal return→调用点接收变量。
> - `RangeFlowNode` 新增 `uint8_t callDepth`，`MAX_CALL_DEPTH=4` 限制展开层数；配合 `enqueue` 的 widening + "状态变化才入队" 防发散。
> - **实现差异/限制**：缓冲区根+累积偏移随参数透传，但**索引表达式区间在被调函数内为局部求解**——索引来自形参时为域内 `TOP`，故多调用点合并后报 **MAY**（保守、不漏报）。回归用例 `interproc_oob.c` 验证。

### 3.3 方案 B（中期，高精度）：迁移到 SVFG 做上下文敏感分析　**`[未实现 — 后续演进]`**
完全对标 SABER：用稀疏值流图（SVFG）做过程间、上下文敏感（`k-callsite`）传播。

```
1. 复用 SrcSnkDDA::initialize() 的构图：
     AndersenWaveDiff → SVFGBuilder.buildPTROnlySVFG/FullSVFG
2. Source = 缓冲区分配点（alloc / stack array）的 SVFG 节点
3. 在 SVFG 上做带 ContextCond 的 forward 传播，
   节点状态 = (根缓冲区, 字节偏移区间)，沿 IntraVFG/CallVFG/RetVFG 边流动
4. 遇到 GepVFGNode / memcpy 调用 → 用 RangeAnalysis 判定越界
```

- **优点**：上下文敏感、按需稀疏传播、与 SABER 框架统一，精度与可扩展性最佳。
- **缺点**：改造量大，需让 `BufferOverflowChecker` 继承 / 复用 `SrcSnkDDA` 的图与遍历基础设施。

### 3.4 推荐演进路径
**先做方案 A**（快速打通跨函数 + 加调用深度上限和 widening 防误报/发散），积累测试用例；**再视精度需求迁移到方案 B**（SVFG）。两者可共存：A 作为"快速模式"，B 作为"精确模式"，由命令行选项切换。

### 3.5 关键风险与对策
| 风险 | 对策 |
| --- | --- |
| 过程间环 / 递归导致 worklist 不收敛 | "状态变化才入队" + widening + 调用深度上限 |
| 上下文不敏感的偏移 join 造成误报 | 方案 A 仅报 must-overflow；需要 may 级别时上方案 B 的上下文 |
| 指针别名（同一缓冲区多指针） | 复用 Andersen 指向集，按对象（ObjVar）而非变量聚合偏移 |

---

## 特性 4：符号仿射关系判定（off-by-one 少分配）

### 4.1 问题根因
区间域是**非关系型**（non-relational）的——它只表达"单个变量的取值范围"，无法表达"两个变量同源"这一关系。真实项目（如 OpenHarmony WLAN）中大量 off-by-one 漏洞形如：

```c
rxEapol->buf = malloc(eapol.len - 1);              // BO-4：少分配 1 字节
memcpy_s(rxEapol->buf, eapol.len, src, eapol.len); // 按 len 拷贝 → 溢出
```

这里拷贝长度 `eapol.len` 来自反序列化，数值在区间域里是 `TOP`；分配尺寸 `len-1` 同样含 `TOP`。`checkMemoryOps` 见 `len.isTop()` 即保守跳过 → **漏报**。但其实只要识别出"分配尺寸 = 拷贝长度 − 1"这一**关系**，无需知道 `len` 的具体值即可判定越界。

### 4.2 设计：轻量符号仿射归一化
把标量整型表达式归一化为一组 `AffineTerm{base, offset}`——`base` 是符号基的结构化标识串（空串表示纯常量），`offset` 是整数偏移。当分配尺寸与拷贝长度归一化后**共享同一 base** 时，比较中 base 相消，仅凭 `offset` 即可判定 `分配 < 拷贝`：

```
allocSize 归一化 = base(len) + (-1)
copyLen   归一化 = base(len) + (0)
=> 同 base，offset 比较：-1 < 0  ⇒  分配 < 拷贝  ⇒  越界（TOP 不再是障碍）
```

```cpp
struct AffineTerm { std::string base; long long offset; };
// 沿 def-use 遍历 Copy / Load(+store 跟随) / Gep / BinaryOp(Add/Sub 常量) / Phi / Select
std::vector<AffineTerm> analyzeAffine(const SVFVar* var);
// 把指向同一源码位置的 SSA 值规范化为相同 token（跨指令匹配，免指向分析）
std::string locationToken(const SVFVar* var);
```

### 4.3 改造点
- `RangeAnalysis.{h,cpp}`：新增 `analyzeAffine(var)`（带 memoization + 深度上限防环）与 `locationToken(var)`。
- `HeapAllocationHandler.{h,cpp}`：新增 `getAllocSizeOperand(call)`，按 `AllocSpec` 返回分配尺寸/计数操作数符号（数值 `TOP` 时仍可用）。
- `BufferOverflowChecker.{h,cpp}`：
  - `initialize` 新增 `allocResultSym`/`allocAddrSym` 旁路表 + `recordAllocSizeSym`，数值尺寸为 `TOP` 时**仍记录**分配尺寸符号，键既按分配结果 var，也按其被存入的字段/槽的 `locationToken`（供后续 load 恢复尺寸）。
  - `checkMemoryOps` 在 `len.isTop()` 跳过**之前**插入 `trySymbolicUnderAlloc` 补救分支：经 `findAllocSizeForBuffer` 取回缓冲区的分配尺寸符号，对 `allocVar` 与 `lenVar` 各求 `analyzeAffine`，对共享 base 比较偏移，证明越界则分级报告。

### 4.4 关键工程点
- **SVF 未建模的分配器**：`OsalMemCalloc` 被 BOF 登记为分配器，但 SVF 核心**不**将其建模为堆对象（无 `AddrStmt→HeapObjVar`、无 `CallPE`），其结果是普通值结点。需额外增设一趟 `pag->getCallSiteSet()` 遍历，用 `getRetICFGNode()->getActualRet()` 取回结果符号并 `recordAllocSizeSym`——这是 BO-5 能命中的前提（BO-4 用 `malloc`，本就被建模）。
- **MUST/MAY 分级**：少分配在条件分支（偏移区间含 0，如 `[-1,0]`）→ MAY；恒定少分配（偏移恒负）→ MUST，与"复杂分支条件触发"语义一致。
- **零新增误报保证**：仅当两侧 base **指针严格相等**时触发，不做任何别名推断；等长拷贝（偏移均为 0，如同函数内全长拷贝）判定为不越界。

### 4.5 验收
- `malloc(len-2)` + `memcpy_s(buf, len, .., len)` → MUST（恒定少分配）。
- 分支上 `malloc(len-1)` + `memcpy_s(buf, len, .., len)` → MAY（条件少分配）。
- `malloc(len)` + `memcpy_s(buf, len, .., len)` → 不报（等长安全，零 FP）。

### 4.6 `[实现状态]` ✅ 已实现
- 新增 `RangeAnalysis::analyzeAffine/locationToken`、`HeapAllocationHandler::getAllocSizeOperand`、`BufferOverflowChecker::recordAllocSizeSym/findAllocSizeForBuffer/trySymbolicUnderAlloc` 及 `allocResultSym/allocAddrSym` 旁路表。
- 新增一趟 `getCallSiteSet()` 遍历捕获 SVF 未建模平台分配器（`OsalMemCalloc`）的尺寸符号。
- 回归用例 `tests/basic_tests/alloc_offbyone_memcpy_s.c`（MUST=1/MAY=1/安全 1），简单版 9/9 全通过、原 8 项零退化。
- 困难用例 OpenHarmony WLAN：BO-4、BO-5 新命中，BO 召回 4/8 → 6/8（25% → 75%），其它困难用例计数不变，零新增误报。详见 `tests/advanced_tests/ANALYSIS.md`。
- **残留（超出本次轻量范围）**：BO-2（结构体定长数组字段越界，需字段敏感）、BO-6（`calloc(num-1, IFNAMSIZ)` 循环按 `num` 写入，含乘法因子，轻量仿射暂不支持乘法比较）。

---

## 特性 5：LLM 辅助 MAY-triage（循环归纳越界分流）

> 目标：在**不改动 sound 区间分析任何判定**的前提下，对静态分析退化为 **MAY** 的循环归纳越界（典型 `int a[10]; for(i=0;i<=10;i++) a[i]`，下标退化为 `TOP`）抽取**最小完备代码切片**，交大模型判定，把 MAY 进一步分流为「疑似越界（LLM_SUSPECT）/ 维持 MAY」。该层为**纯叠加（overlay）**。

### 5.1 问题根因
区间域无法建模循环守卫 `i <= 10`：`RangeAnalysis::analyzeVarRange` 处理 `Load/Store/Copy/BinaryOp/Phi/Select`，但**不处理 `CmpStmt/BranchStmt`**，循环回边 join 后归纳变量被 widening 抬到 `TOP`，下标无界 → 访问只能报 **MAY**（保守不漏报，但精度不足、噪声大）。完全在区间域内补关系判定（如循环不变式）工程量大；本特性改用「**抽取结构化切片 + 大模型语义判定**」的低成本路径。

### 5.2 设计：纯 overlay 的三段式
```
flushReports 发射阶段（幸存 MAY）
  └─ 命中「GEP_OOB 且 offset.isTop() 且存在非常量下标 indexVar」
       └─ LLMTriage::collectSlice 抽取切片（只读 IR）
            access(file/line/col/base/index_expr/index_range_static)
            buffer(capacity/is_heap/domain)
            induction(var/init/step/update_op，尽力而为)
            guards[]（谓词/两操作数/界区间，CmpStmt 结构化 token 关联）
            code_snippet（访问点上下文源码窗口）
       └─ 始终写 bof_slices.json（API 为空兜底 + schema 自验）
       └─ 若配置 LLM 端点 → std::system 调 Python sidecar → 读回 bof_verdicts.json
            verdict=OUT_OF_BOUNDS 且 confidence≥threshold → 终端标注 LLM_SUSPECT
            verdict=SAFE → 仅降展示优先级（MAY 保留）
            UNKNOWN/sidecar 失败 → 维持 MAY
```

### 5.3 soundness 硬约束（最重要）
- LLM **永不**把 MAY 直接清成 SAFE：只允许「**升级**为 LLM_SUSPECT 标注」或「**维持** MAY」。
- sound 报告（`SVFBugReport`/`bof-report` JSON）**完全不变**；triage 结果只出现在**终端标注**与新增的 `bof_slices.json`/`bof_verdicts.json` 两个 overlay 文件中。
- 最坏情况只多报、绝不漏报。从机制上保证「triage 层不污染 sound 结果」。

### 5.4 C++ / Python 解耦
- C++ 仅负责**切片导出**与**读回裁决**，不内嵌任何 HTTP 依赖（SVF 工具链无 curl）。
- 实际 API 调用隔离到 Python sidecar `svf-llvm/tools/BOF/llm_triage.py`（标准库 `urllib`，OpenAI 兼容 `/chat/completions`，`temperature=0` 利于论文可复现）。
- API Key 经**环境变量** `BOF_LLM_API_KEY` 传给子进程（不落命令行）；`api_url/model` 同理。
- 任何失败（非零退出 / 缺文件 / 解析失败）一律按「无裁决」处理，保持 MAY，不阻塞、不改变 sound 结论。

### 5.5 切片字段清单（`bof-slice/v1`）
| 分组 | 字段 | 来源 |
| --- | --- | --- |
| access | file/line/col | `ICFGNode::getSourceLoc()` 解析 `{ln,cl,fl}` |
| access | base | GEP 结果指针友好名 |
| access | index_expr | `RangeAnalysis::analyzeAffine(indexVar)` 符号仿射形 |
| access | index_range_static | `analyzeVarRange(indexVar)`（通常 TOP） |
| buffer | capacity | `getBufferRange(base)`（发射时的 size） |
| buffer | is_heap / domain | `elements`/`bytes` |
| induction | var/init/step/update_op | 尽力而为：扫 `BinaryOPStmt(Add/Sub)` 经 `locationToken` 关联归纳槽取步长 |
| guards[] | predicate/lhs/rhs/rhs_range | 扫 `CmpStmt`，operand 的 `locationToken` 与下标 token 相等即关联 |
| - | code_snippet | 读源码文件，访问行上方 8 行 + 下方 2 行（覆盖 `for` 头） |

> Range 统一以 `{lower,upper,is_top,is_bottom}` **字符串**编码，`+INF/-INF` 用字符串规避非有限数值进 JSON 的问题。

### 5.6 关键技术决策
- **切口选择「幸存 + TOP + GEP_OOB」**：精准命中循环退化场景，控制 LLM 调用量与噪声，对已能判定的访问零开销（热路径不触发）。
- **守卫关联是唯一新增遍历**：复用既有 `CmpStmt::getPredicate()/getOpVar`、`BinaryOPStmt::getOpcode()`，以 `RangeAnalysis::locationToken` 做结构化 token 匹配（-O0 下 `a[i]` 与 `i<=10` 是同一槽的两次 load，token 相同），无需指向分析。守卫关联失败时 `guards=[]`（仍导出切片，交 LLM/人工 + code_snippet 判定）。
- **复用既有切片基建**：`analyzeAffine()`（索引符号式）、`locationToken()`（结构标识）、`getBufferRange()`（容量）、`getSourceLoc()`（源码位置）。
- **解析复用 cJSON**：config / verdicts 解析复用 SVF 自带 `Util/cJSON`；slices 输出为手写最小转义序列化（可控、与现有字符串处理风格一致）。

### 5.7 配置与命令行
```
-llm-config=<file>     # JSON: {api_url, api_key, model, threshold, sidecar, slice_out, python}
-llm-slice-out=<file>  # 切片导出路径（默认 bof_slices.json，始终写出）
-llm-sidecar=<py>      # Python sidecar 路径；为空 => API 为空模式（仅导出切片）
# 端点回退：环境变量 BOF_LLM_API_URL / BOF_LLM_API_KEY / BOF_LLM_MODEL
# 启用条件 hasApi() := api_url 与 sidecar 均就绪
```

### 5.8 验收
- **API 为空**：`bof a.ll -llm-slice-out=...` 仍生成 `bof_slices.json`，schema 字段齐全（schema/capacity/guards/induction/code_snippet/index_range_static）。
- **mock sidecar**：`tests/basic_tests/mock_sidecar.py` 返回 OUT_OF_BOUNDS(0.99)，幸存循环 MAY 被标注 `LLM_SUSPECT`。
- **零退化**：既有 9 个基础回归用例 MUST/MAY 计数不变（triage 不改 sound 分类）。

### 5.9 `[实现状态]` ✅ 已实现
- 新增 `svf/include/BOF/LLMTriage.h` + `svf/lib/BOF/LLMTriage.cpp`：切片数据模型（`BofSlice/GuardInfo/InductionInfo/SliceRange`）、`LLMTriageConfig`（文件 + env 加载）、`LLMVerdict`，及 `collectSlice/writeSlices/runSidecarAndLoad`、`extractGuards/extractInduction/readCodeSnippet`。
- `BufferOverflowChecker`：`PendingReport` 增 `indexVar`，经 `handleGep`(取首个 TOP 非常量下标)→`checkAccess`→`reportBufferOverflowError` 透传；`flushReports` 末尾对幸存 `TOP MAY GEP_OOB` 收切片→始终写 `bof_slices.json`→有配置则跑 sidecar 读回→按阈值终端标注。新增 `setLLMTriageConfig`。
- `svf-llvm/tools/BOF/bof.cpp`：新增 `-llm-config/-llm-slice-out/-llm-sidecar`，`runOnModule` 前注入 `LLMTriageConfig`（含 env 回退）。
- `svf-llvm/tools/BOF/llm_triage.py`：OpenAI 兼容 sidecar，`temperature=0`，按 `bof-verdict/v1` 写 `bof_verdicts.json`；失败降级 UNKNOWN。
- 回归：新增 `tests/basic_tests/loop_oob.c`（off-by-one 循环，MUST=0/MAY≥1）+ `mock_sidecar.py`，`run_tests.sh` 增 API-空 schema 校验与 mock-sidecar `LLM_SUSPECT` 断言。
- **构建附带修复**：新增头文件触发 `SvfCore` 全量重编，暴露既有潜在告警 `SABER/FileChecker.h` 的 `-Werror=inconsistent-missing-override`，补 `override` 修复（与本特性无关的latent error，最小修正）。
- **范围限定（v1）**：仅切片「幸存 MAY + GEP_OOB + 下标 TOP」的循环访问；memcpy 等其他类型暂不纳入。`enter_loop_when` 暂置 `unknown`（谓词 + rhs + code_snippet 已足以让 LLM 判定）。

---


为同时支撑三项特性，建议对核心结构做如下增量（向后兼容）：

```cpp
// RangeFlowNode 扩展
class RangeFlowNode {
    const SVFVar* base;
    const SVFVar* parent;          // 缓冲区根对象（对象级聚合）
    Range accumulate_offset;       // 统一为“字节偏移”
    bool  isHeap;
    uint8_t callDepth = 0;         // 新增：跨函数展开深度上限控制
};

// 报告扩展：区分错误来源 + 源码位置 + 严重级别
enum class BofKind { GEP_OOB, MEMCPY_OOB, MEMSET_OOB, STRCPY_OOB };
void reportBufferOverflowError(..., BofKind kind, const ICFGNode* loc,
                               bool mustOverflow);
```

并新增三个注册表 / 处理器（与现有类平级，保持单一职责）：
- `AllocAPIRegistry`（特性 1）
- `MemCopyAPIRegistry` + `checkMemoryOps()`（特性 2）
- `propagate` 内 `handleCall/handleRet`（特性 3 方案 A）或 `SVFGBofSolver`（方案 B）
- `RangeAnalysis::analyzeAffine/locationToken` + `allocResultSym/allocAddrSym` 旁路表 + `trySymbolicUnderAlloc`（特性 4）
- `LLMTriage`（切片抽取 + 导出 + sidecar）+ `PendingReport.indexVar` 透传 + `llm_triage.py`（特性 5，纯 overlay）

---

## 五、实施顺序建议

1. **特性 1（分配识别统一）** — 独立、风险低，先做，为后续提供准确的缓冲区根与尺寸。✅ 已完成
2. **特性 2（内存拷贝检查）** — 依赖特性 1 的缓冲区尺寸，规则表清晰、收益直观。✅ 已完成
3. **特性 3（跨函数）方案 A** — 在前两者基础上打通过程间，配合 widening。✅ 已完成
4. **特性 4（符号仿射少分配）** — 在前三者基础上叠加轻量关系判定，攻克同源 off-by-one 漏报。✅ 已完成
5. **特性 5（LLM 辅助 MAY-triage）** — 在 sound 区间分析之上叠加纯 overlay，对循环归纳 MAY 抽切片交大模型分流。✅ 已完成
6. （可选）**特性 3 方案 B（SVFG）** — 精度升级，作为长期目标。⏳ 未实现（保留演进）

> **落地补充**：在三特性之前已先行落地《BOF模块评价与改进建议.md》全部 P0 项（`PhiStmt`/不动点+widening/`size>=1`/报告去重与源码位置），否则跨函数与循环场景的精度无法保证。配套回归用例位于 `tests/basic_tests/`（简单版 9/9）与 `tests/advanced_tests/`（真实项目困难用例），经 `setup.sh`+`build.sh` 全量编译通过（日常用 `build_bof.sh` 增量）并验证。
