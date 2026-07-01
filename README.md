# SVFmem+

`SVFmemplus` 面向 LLVM bitcode（`.bc`）执行内存缺陷静态分析，并输出可供人工阅读和下游程序消费的增强告警报告。

## 功能

Saber 当前支持：

- `-leak`：内存泄漏（`NeverFree`、`PartialLeak`）
- `-dfree`：重复释放（`DoubleFree`）
- `-uaf`：释放后使用（`UseAfterFree`）
- `-uninit`：未初始化使用（`Uninitialized Use`）

`bof` 工具用于检测缓冲区越界（`BufferOverflow`）。本轮报告格式优化只作用于 Saber，尚未改造 BOF。

每条告警输出为独立 JSON。除 leak 外，`path` 是一条裁剪后的 SVFG
值流 witness；leak 的 `paths` 是可能安全释放对象的路径，
`leak_condition` 表示安全条件并集的补集。

## 构建

已验证的 LLVM 21 Docker 构建命令：

```bash
docker run --rm \
  -v "/home/xyc/openEuler分析流程/SVFmemplus":/SVFmemplus \
  -w /SVFmemplus \
  nf-image:llvm21 \
  bash -lc 'source ./build.sh'
```

在依赖已经满足的主机或容器中也可直接执行：

```bash
source ./build.sh
source ./setup.sh
```

## Saber 默认报告

使用 `-report-dir` 指定输出根目录：

```bash
saber -leak   -report-dir=/path/to/output input.bc
saber -dfree  -report-dir=/path/to/output input.bc
saber -uaf    -report-dir=/path/to/output input.bc
saber -uninit -report-dir=/path/to/output input.bc
```

四类检查器分别写入：

```text
alerts/memory_leak/<sha256>.json
alerts/double_free/<sha256>.json
alerts/use_after_free/<sha256>.json
alerts/uninit_use/<sha256>.json
```

`-report-dir` 默认值为当前目录。终端内容仅作为运行日志，不是下游输入。

## 统一运行（全局管线）

在仓库根目录配置 `script/config.env` 后：

```bash
./script/run_svf.sh                              # Step1 静态分析
./script/run_pipeline.sh                         # SVF + FPhandler
./script/run_svf.sh --checkers leak,dfree        # 仅跑指定 checker
```

`defect_types=leak,dfree,uaf,uninit` 控制运行哪些检查器；追加 `bof` 可启用 BOF。

## 语义规则反馈

Saber 可加载经人工审核批准的 `semantic-rules/v1` 规则：

```bash
saber -uninit \
  -saber-semantic-rules=/path/to/semantic_rules.approved.json \
  -report-dir=/path/to/output \
  input.bc
```

这为后续 FPhandler 将 LLM 研判中发现的函数语义反馈给静态分析器预留了稳定接口。只有状态为 `approved` 的规则会被 Saber 使用。

## 与 FPhandler 联动

将 Saber 的输出目录配置为 FPhandler 的 `OUTPUT_DIR`。FPhandler 会：

1. 从 `alerts/` 递归发现单警报 JSON；
2. 直接使用警报 path、源码上下文和 checker 证据；
3. 将 `classification` 与 `reason` 原子写回同一文件。

## 主要代码

- `svf-llvm/tools/SABER/saber.cpp`：Saber 入口和报告参数
- `svf/lib/SABER/`：各 Saber 检查器及报告实现
- `svf-llvm/tools/GraphReader/`：语义查询服务
- `svf-llvm/tools/BOF/`、`svf/lib/BOF/`：缓冲区越界检测
- 仓库根 `script/run_svf.sh`：全局管线静态分析入口
