# SVFmem+

To provide detection support for use-after-free (UAF), undefined usages, and array out-of-bounds within the SABER framework of SVF (a static analysis tool).

## 模块定位

`SVFmemplus` 是交付件2“内存缺陷检测与细粒度程序语义记录工具”的源码实现，面向 LLVM bitcode（`.bc`）执行静态分析并输出缺陷告警。

## 功能概览

### 内存缺陷检测（5类）

- `-leak`：内存泄漏（`NeverFree`、`PartialLeak`）
- `-dfree`：重复释放（`DoubleFree`）
- `-uaf`：释放后使用（`UseAfterFree`）
- `-uninit`：未初始化使用（`Uninitialized Use`）
- `bof` 工具：缓冲区越界（`BufferOverflow`）

对应入口与检查器：

- `svf-llvm/tools/SABER/saber.cpp`
  - `svf/lib/SABER/LeakChecker.cpp`
  - `svf/lib/SABER/DoubleFreeChecker.cpp`
  - `svf/lib/SABER/UseAfterFreeChecker.cpp`
  - `svf/lib/SABER/UninitChecker.cpp`
- `svf-llvm/tools/BOF/bof.cpp`
  - `svf/lib/BOF/BufferOverflowChecker.cpp`

### 细粒度程序语义记录

`graph-reader` 在加载 bitcode 后构建 `SVFIR/ICFG/SVFG`，以常驻进程方式接收 JSON 命令并返回 JSON 结果，可用于查询：

- 函数体、调用关系、条件路径
- 形参/实参/返回值相关值流节点
- 关键 `SVFG` 节点信息与值路径
- 与 `free` 相关的调用闭包和距离
- 指定源码行的宏上下文

核心代码位于 `svf-llvm/tools/GraphReader/`。

## 构建与环境

### Docker 镜像（推荐）

工作区统一 SVF 容器镜像为 **`svf-llvm21`**（LLVM 21.1.0），定义见本目录 `Dockerfile`：

```bash
docker build -t svf-llvm21 -f SVFmemplus/Dockerfile SVFmemplus/
```

在容器内构建 SVFmemplus：

```bash
bash build_common/prepare_svf_docker.sh \
  --svf-dir /path/to/SVFmemplus \
  --svf-image svf-llvm21
```

### 主机 / 交付环境脚本

请参考交付目录中的环境脚本（`linuxUbuntu环境` 与 `openEuler环境`）。在容器或主机环境满足依赖后：

```bash
cd SVFmemplus
./build.sh
```

构建完成后加载环境变量：

```bash
source ./setup.sh
```

若运行时缺少 `libz3.so.4`，可按实际路径建立软链接：

```bash
ln -s /src/SVFmemplus/z3.obj/bin/libz3.so /usr/lib/libz3.so.4
```

## 使用方式

### SABER（泄漏/双重释放/UAF/未初始化）

```bash
saber <option> <input.bc>
```

示例：

```bash
saber -leak demo.bc > demo-leak.txt
saber -dfree demo.bc > demo-dfree.txt
saber -uaf demo.bc > demo-uaf.txt
saber -uninit demo.bc > demo-uninit.txt
```

### BOF（缓冲区越界）

```bash
bof <input.bc> > demo-bof.txt
```

### GraphReader（语义查询）

```bash
graph-reader -stat=false <input.bc>
```

启动后会输出 `graphreader-initialized`，随后可按行输入 JSON 命令进行查询。
