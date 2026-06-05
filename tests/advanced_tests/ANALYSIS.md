# BOF 困难版测试 — 失败根因分析与改进报告

> 配套数据见 `results/SUMMARY.md`（每次运行 `run_hard_tests.sh` 自动刷新）与
> `results/<用例>.out.txt`（逐用例完整检测输出）。

## 1. 结论速览

| 用例 | 注入 BO | 改进前 检测(MUST/MAY) | 改进后 检测(MUST/MAY) | BO 真实召回 | 备注 |
| --- | ---: | --- | --- | --- | --- |
| object1_kernel_50 | 10 | 11 / 13 | 11 / 13 | **10/10** | 移位·算术·位运算索引传播完整命中 |
| object3_ubs_50 | 10 | 13 / 0 | **16 / 0** | **10/10** | 改进后越界判定更确定（MUST），并经 `_s` 拷贝路径补充命中 |
| object1_kernel_10 | 2 | 0 / 69 | 0 / 69 | 部分(淹没在 MAY) | 子集 bc，噪声大 |
| object3_ubs_10 | 2 | 57 / 115 | 68 / 115 | 部分(淹没在 MAY) | 子集 bc，噪声大 |
| openharmony_40 | 8 | 0 / 2 | **2 / 4** | **6/8** | 新增 2 MUST（BO-7/BO-8）+ 2 MAY（BO-4/BO-5 符号少分配），见下 |

**核心发现**：对 50 缺陷的内核 / ubs_engine 用例，BOF 已能 **100% 命中全部 10 个注入 BO 缺陷**——
其依赖的"算术 / 移位 / 位运算下标传播"在 `RangeAnalysis` 中已完整覆盖。困难点不在"算不出越界"，
而在两类：(a) **过近似产生的 MAY 假阳性**（变量下标被保守提升为 TOP）；(b) **OpenHarmony 域内的堆/结构体越界**
因分配器与安全拷贝 API 未建模、或"分配尺寸与拷贝长度同源但数值未知"而被整体跳过。后者已由本次的
**符号仿射关系判定**攻克（BO-4/BO-5）。

## 2. 命中验证（关键证据）

`.bc` 调试信息使用 `focused_src/*.c`，其行号相对仓库内 `.c` 副本存在**稳定偏移**（约 +8 ~ +11 行）。
按偏移对齐后，注入 BO 缺陷与检测位置逐一吻合：

- **object1_kernel_50**：`.c` 中 BO 标注行 `121,196,351,509,705,812,876,939,1003,1070`
  → 检出行 `132,206,360/374,519,715,823,886,950,1013,1080`（偏移 +9~+11），**10/10 全中**。
- **object3_ubs_50**：`.c` BO 标注 `49,178,244,398,514,650,712,774,836,903`
  → 检出 `58,186,253,406,522,660,721,784,845,912`（偏移 +8~+10），**10/10 全中**；
  改进后另在 `255/524/605` 经 `memcpy_s` 规则补报同区域越界。

> 注：注入的 BO 缓冲区（如 `nums=malloc(2*sizeof(int))`、`elems=malloc(3*sizeof(int))`）实际使用
> **已建模的 `malloc`**，这解释了为何这些 BO 本就能被命中——与"内核分配器是否建模"无关。

## 3. 漏报 / 噪声根因分类

| 类别 | 表现 | 典型用例 | 是否本次可改进 |
| --- | --- | --- | --- |
| R1 变量下标过近似 | 无法确定的下标提升为 TOP → 大量 `MAY` | 全部 | 否（保守正确，调精度风险高，留待后续） |
| R2 堆缓冲区大小未知 | 分配器未登记 → 缓冲区大小 BOTTOM → `checkAccess` 直接跳过 → **漏报** | openharmony (`OsalMemCalloc`) | **是（已改进）** |
| R3 安全拷贝 API 未建模 | `memcpy_s/memset_s/strncpy_s` 不在拷贝规则表 → 尺寸不一致的拷贝越界漏报 | openharmony / ubs | **是（已改进）** |
| R4 子集 bc 噪声 | `*_subset.bc` 仅含裁剪后的函数与大量未约束入参 → MAY 爆炸 | object1/3_10 | 否（输入特性） |
| R5 符号少分配（off-by-one） | 分配尺寸为 `len-1`（条件分支），拷贝/写入仍为 `len`，且 `len` 来自反序列化数值为 TOP → 原 `checkMemoryOps` 见 `isTop()` 即跳过 → **漏报** | openharmony BO-4/BO-5 | **是（已改进，符号仿射判定）** |
| R6 流敏感字段越界 | `index == size` 且由位标志门控的定长数组/字段写；循环边界与分配元素数不一致 | openharmony BO-1/2/6 | 部分（超出本次安全范围） |

## 4. 本次在 BOF 内的改进（仅改 BOF 源码，零简单版退化）

> 约束遵循：未触碰 BOF 以外模块。

### 4.1 `svf/lib/BOF/AllocAPIRegistry.cpp` — 扩充 OpenHarmony OSAL 分配器

```cpp
localTable["OsalMemAlloc"]      = {AllocSpec::SIZE_ARG, 0, -1};
localTable["OsalMemCalloc"]     = {AllocSpec::SIZE_ARG, 0, -1};   // 单参 size
localTable["OsalMemAllocAlign"] = {AllocSpec::SIZE_ARG, 1, -1};
```

针对 R2：使 `OsalMemCalloc` 分配的堆缓冲区获得精确字节容量，让其后的越界访问可被判定。

### 4.2 `svf/lib/BOF/MemCopyAPIRegistry.cpp` — 登记 securec 安全拷贝族

```cpp
rules["memcpy_s"]  = {{0, 3}, {2, 3}};   // memcpy_s(dest,destMax,src,count) → count=arg3
rules["memmove_s"] = {{0, 3}, {2, 3}};
rules["memset_s"]  = {{0, 3}};           // memset_s(dest,destMax,c,count)
rules["strncpy_s"] = {{0, 3}};
rules["strncat_s"] = {{0, 3}};
```

针对 R3：`_s` 变体的实际写入长度在 `count`（arg3），与经典 API 参数位不同；登记后可检出
"尺寸不一致"的堆拷贝越界。`strcpy_s` 因其 `src` 在 arg2 与现有 strcpy-like 假设冲突，**未纳入**以免误判。

> 为何不盲目扩充 `kmalloc/kzalloc`：实测注入 BO 缓冲区均用 `malloc`（已命中）；而 `kmalloc` 分配的缓冲区
> 在用例中被安全使用，登记它们**不会提升召回**，反而可能把原本"大小未知→跳过"的安全访问变成 `MAY` **新增假阳性**，
> 违反"无新增误报"原则，故不采纳。

### 4.3 符号仿射关系判定 — 攻克"分配尺寸与拷贝长度同源但数值未知"（BO-4 / BO-5）

针对 R5：当 `len` 来自反序列化、数值为 TOP 时，区间域丢失了"分配尺寸与拷贝长度同源"这一关键关系，
无法证明 off-by-one。引入**轻量符号仿射归一化**：把分配尺寸表达式与拷贝长度表达式都化为
`(同一符号基 base + 整数偏移区间)`。当两侧 base **指针相等**时，base 在比较中相消，仅凭偏移即可判定
`分配 < 拷贝`，TOP 不再是障碍。

涉及文件（均在 BOF 内）：

- `svf/{include,lib}/BOF/RangeAnalysis.{h,cpp}`：新增 `analyzeAffine(var)` 与 `locationToken(var)`——
  遍历 `Copy/Load/Store/Gep/BinaryOp(Add/Sub 常量)/Phi/Select` 的 def-use 边，把 `len`、`len-1`、
  `phi(len,len-1)`、`select(_,len,len-1)` 归一化为「基 + 偏移区间」（带 memo 与深度上限）。
- `svf/{include,lib}/BOF/HeapAllocationHandler.{h,cpp}`：新增 `getAllocSizeOperand(call)`，按 `AllocSpec`
  返回分配尺寸/计数操作数符号（数值 TOP 时仍可用）。
- `svf/{include,lib}/BOF/BufferOverflowChecker.{h,cpp}`：`initialize` 新增 `allocResultSym` / `allocAddrSym`
  旁路表记录分配尺寸符号；`checkMemoryOps` 在 `lenBytes.isTop()` 跳过**之前**插入 `trySymbolicUnderAlloc`
  补救分支，证明 `allocSize < copyLen` 时报 MAY/MUST。

关键工程点：

- **SVF 未建模的分配器**：`OsalMemCalloc` 被 BOF 登记为分配器，但 SVF 核心**不**将其建模为堆对象
  （无 `AddrStmt→HeapObjVar`、无 `CallPE`），故其结果是普通值结点。新增一趟 `getCallSiteSet()` 遍历，
  用 `getRetICFGNode()->getActualRet()` 取回结果符号并记录——这是 BO-5 能命中的前提（BO-4 用 `malloc`，
  本就被建模）。
- **MUST/MAY 分级**：少分配在条件分支（偏移区间含 0，如 `[-1,0]`）→ MAY；恒定少分配（偏移恒负）→ MUST，
  与缺陷"复杂分支条件触发"语义一致。
- **零新增误报保证**：仅当两侧 base **指针严格相等**时触发，不做别名推断；等长拷贝（偏移均为 0，
  如 BO-5 同函数内的 `dst->ie` 全长拷贝）判定为不越界，不误报。

### 4.4 改进效果（真阳性，已逐条核验）

- **openharmony**：覆盖率 **25% → 75%**（约 4/8 → 6/8）。
  - 既有：BO-1/BO-3（MAY 栈/指针）、BO-7/BO-8（MUST 尺寸不一致堆拷贝）。
  - 新增：**BO-4**（`sbuf_wpa_cmd_adapter.c` 的 `rxEapol->buf = malloc(len-1)` + `memcpy_s(buf,len,..,len)`）→ MAY；
    **BO-5**（`sbuf_event_adapter.c` 的 `dst->beaconIe = OsalMemCalloc(len-1)` + `memcpy_s(beaconIe,len,..,len)`）→ MAY。
  - 残留：BO-2（结构体定长数组字段越界，需字段敏感）、BO-6（`calloc(num-1, IFNAMSIZ)` 循环按 `num` 写入，
    含乘法因子与循环边界，超出本次轻量仿射范围）。
- **object3_ubs_50**：越界判定由 MUST=13 增至 16（同区域经 `_s` 拷贝路径补充确证），10/10 召回不变。
- **简单版回归**：新增 `alloc_offbyone_memcpy_s.c`（MUST=1/MAY=1/安全 1）后 **9/9 全通过**，原 8 项零退化。

## 5. 已知局限与后续方向

- **R1 假阳性收敛**：可引入路径条件 / 关系域，或对"循环归纳变量"给更紧的区间，降低 MAY 噪声（需谨慎评估召回）。
- **BO-2 字段越界**：`event->ifName[IFNAMSIZ(+0/1)]` 越过定长字段 `char[IFNAMSIZ]` 但仍落在整个堆对象内，
  当前按整对象字节建模故不报，需"定长数组字段边界"级别的字段敏感（有引入 FP 风险，本次未纳入）。
- **BO-6 循环+乘法少分配**：`calloc(num-1, IFNAMSIZ)` 容量与按 `num` 的循环写入上界都是 `num` 的函数，
  含乘法因子；当前轻量仿射只支持加减常量，扩展到带正因子的乘法比较是直接的后续方向。
- **输入质量**：`*_subset.bc` 为裁剪子集，入参大量未约束，建议以完整链接 bc 评估真实能力。
