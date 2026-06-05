# BOF 简单版回归测试用例

本目录（位于 `SVFmemplus/tests/basic_tests`，与 `svf/`、`svf-llvm/` 源码目录分离，实现测试与开发解耦）
提供 BOF（缓冲区溢出 / 数组越界）检测器的最小回归用例，覆盖：栈数组、堆缓冲区、
跨函数指针透传、内存拷贝/字符串 API、单字节缓冲区、结构体字段、符号少分配七类场景。

> 真实项目的"困难版"用例见同级目录 `../advanced_tests/`。

## 用例与预期

| 文件 | 场景 | 预期（MUST 必然越界） | 安全用例（无报告） |
| --- | --- | --- | --- |
| `stack_oob.c` | 栈数组 | `write_oob` 写 `a[10]`、`read_oob` 读 `a[16]`（`int a[10]`） | `safe` 访问 `a[9]`（循环归纳变量报 MAY，保守） |
| `heap_oob.c` | 堆缓冲区 | `malloc(16)→p[16]`、`calloc(4,4)→p[4]`、`realloc(p,4)→p[7]` | `heap_safe` 访问 `p[15]` |
| `interproc_oob.c` | 跨函数（方案 A + k=1 上下文） | `write_at(a,11,..)` 与 `write_at(a,10,..)` 经实参→形参传播后在 `p[idx]` 越界（k=1 调用点上下文精确恢复常量索引，分别报 MUST） | `interproc_safe` 传 `9` |
| `complex_index.c` | 复杂索引（传 `a[]`、`b`、`c`，函数内访问 `a[b*c]`） | `write_mul(g,4,3)→g[12]`、`write_mul(g,2,5)→g[10]`、`write_affine(m,5,3)→m[b*c+b=20]`（多形参派生索引经 k=1 绑定精确恢复，按调用点各自报 MUST） | `write_mul(g,3,3)→g[9]`、`write_affine(m,3,4)→m[15]` |
| `complex_heap.c` | 堆缓冲区 + 计算索引 `a[b*c]`（字节域换算） | `store_at(p,4,4)→p[16]`（字节 64）、`store_at(q,3,3)→q[9]`（字节 36）越界报 MUST | `store_at(p,3,3)→p[9]`、`store_at(q,2,3)→q[6]` |
| `memcpy_oob.c` | 内存拷贝 / 字符串 | `memcpy(dst[8],..,16)`、`memset(buf[8],0,16)`、`strncpy(dst[4],..,16)` | `copy_safe` `memcpy(dst[16],..,16)` |
| `single_byte.c` | 单字节缓冲区 | `char b[1]` 写 `b[1]`（`size>=1` 边界修复） | 写 `b[0]` |
| `struct_oob.c` | 结构体字段 | `struct{...; char data[8];}` 写 `data[8]`（GEP 字段扁平化越界） | 写 `data[7]` |
| `alloc_offbyone_memcpy_s.c` | 符号少分配（仿射关系判定） | `const_under`：`malloc(len-2)`+`memcpy_s(buf,len,..,len)` 恒定少分配报 MUST；`cond_under`：分支上 `malloc(len-1)` 条件少分配报 MAY（`len` 数值为 TOP，但分配尺寸与拷贝长度同源 `len`，base 相消后凭偏移证明越界） | `exact_alloc`：`malloc(len)`+`memcpy_s(buf,len,..,len)` 等长安全 |

### MUST / MAY 基线

| 文件 | MUST | MAY |
| --- | --- | --- |
| `stack_oob.c` | 2 | 1 |
| `single_byte.c` | 1 | 0 |
| `heap_oob.c` | 3 | 0 |
| `memcpy_oob.c` | 3 | 0 |
| `interproc_oob.c` | 2 | 0 |
| `complex_index.c` | 3 | 0 |
| `complex_heap.c` | 2 | 0 |
| `struct_oob.c` | 1 | 0 |
| `alloc_offbyone_memcpy_s.c` | 1 | 1 |

> 符号少分配检查：当拷贝/写入长度数值未知（TOP）但与分配尺寸表达式同源（共享同一符号基 `len`）时，
> 将两侧归一化为「基 + 整数偏移」，base 在比较中相消，仅凭偏移即可证明 `分配 < 拷贝`。
> 恒定少分配（偏移恒为负）报 MUST；条件少分配（偏移区间含 0）报 MAY；等长不报。
> 该机制严格要求 base 指针相等才触发，零新增误报。

> 说明：内存拷贝检查在“缓冲区存储域”内进行——堆缓冲区按字节精确；栈 `char[]` 缓冲区按字节精确；
> 非 char 的栈数组按元素字节大小换算近似。用例统一采用 char 缓冲区以保证字节级精确判定。
> 长度参数未知（TOP）或源串缓冲区未知时按保守策略不报，以避免误报泛滥。

## 运行方法

### 一键回归（推荐）

```bash
./run_tests.sh
```

脚本会自动定位 `clang`（优先 `llvm@16`）与 `bof`（`Release-build/bin/bof` 或 `bin/bof`），
逐个用例编译为 LLVM IR、运行检测器，并将 MUST/MAY 报告数与基线比对，输出 PASS/FAIL 汇总；
全部通过时退出码为 0。可用环境变量覆盖工具路径：

```bash
CLANG=/path/to/clang BOF=/path/to/bof ./run_tests.sh
```

> 生成的 `*.ll` / `*.report.json` 等中间产物已在 `.gitignore` 中忽略。

### 手动运行

1. 先用仓库根的 `../../setup.sh` + `../../build.sh` 完成编译，确保 `bof` 可执行文件已生成（位于
   `Release-build/bin/bof` 或 `bin/bof`，取决于构建配置）。
2. 将 C 用例编译为 LLVM bitcode/IR：

   ```bash
   clang -S -emit-llvm -fno-discard-value-names -g -O0 stack_oob.c -o stack_oob.ll
   ```

   （其余 `.c` 同理。`-g` 保留源码位置，便于报告中的 Location 字段；`-O0` 避免越界访问被优化消除。）

3. 运行检测器：

   ```bash
   ./bof stack_oob.ll
   # 可选：导出结构化 JSON 报告
   ./bof -bof-report=stack_oob.report.json stack_oob.ll
   ```

4. 在终端输出中核对：每个 `*_oob` 场景应出现 `MUST buffer overflow`，安全用例不应产生报告。
