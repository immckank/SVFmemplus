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

## 四、统一落地的数据结构调整

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

---

## 五、实施顺序建议

1. **特性 1（分配识别统一）** — 独立、风险低，先做，为后续提供准确的缓冲区根与尺寸。✅ 已完成
2. **特性 2（内存拷贝检查）** — 依赖特性 1 的缓冲区尺寸，规则表清晰、收益直观。✅ 已完成
3. **特性 3（跨函数）方案 A** — 在前两者基础上打通过程间，配合 widening。✅ 已完成
4. **特性 4（符号仿射少分配）** — 在前三者基础上叠加轻量关系判定，攻克同源 off-by-one 漏报。✅ 已完成
5. （可选）**特性 3 方案 B（SVFG）** — 精度升级，作为长期目标。⏳ 未实现（保留演进）

> **落地补充**：在三特性之前已先行落地《BOF模块评价与改进建议.md》全部 P0 项（`PhiStmt`/不动点+widening/`size>=1`/报告去重与源码位置），否则跨函数与循环场景的精度无法保证。配套回归用例位于 `tests/basic_tests/`（简单版 9/9）与 `tests/advanced_tests/`（真实项目困难用例），经 `setup.sh`+`build.sh` 全量编译通过（日常用 `build_bof.sh` 增量）并验证。
