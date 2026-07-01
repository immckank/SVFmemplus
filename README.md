# SVFmem+

`SVFmemplus` 面向 LLVM bitcode（`.bc`）执行内存缺陷静态分析，并输出可供人工阅读和下游程序消费的增强告警报告。

## 功能

Saber 当前支持：

- `-leak`：内存泄漏（`NeverFree`、`PartialLeak`）
- `-dfree`：重复释放（`DoubleFree`）
- `-uaf`：释放后使用（`UseAfterFree`）
- `-uninit`：未初始化使用（`Uninitialized Use`）

`bof` 工具用于检测缓冲区越界（`BufferOverflow`）。本轮报告格式优化只作用于 Saber，尚未改造 BOF。

增强报告包含稳定告警 ID、源码片段、调用轨迹、路径条件、求解摘要和语义候选等字段。`graph-reader` 仍可作为常驻语义查询后端，提供函数、调用关系、值流、路径和宏上下文查询。

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

Markdown 增强告警和 `saber-report/v2` JSON 均为默认行为，不需要额外开关。使用一个 `-report-dir` 参数统一指定输出目录：

```bash
saber -leak   -report-dir=/path/to/output input.bc
saber -dfree  -report-dir=/path/to/output input.bc
saber -uaf    -report-dir=/path/to/output input.bc
saber -uninit -report-dir=/path/to/output input.bc
```

如果输入文件为 `input.bc`，相应检查器默认产生：

```text
input_<checker>_report.json
input_<checker>_report.md
input_<checker>_slices.json
```

其中 `<checker>` 为 `leak`、`dfree`、`uaf` 或 `uninit`。终端告警仍输出到标准输出，可按需重定向为 TXT。

`-report-dir` 默认值为当前目录。兼容参数 `-saber-slice-out=<file>` 可单独覆盖 slice JSON 的位置；新管线应优先使用 `-report-dir`。

## 统一运行脚本

脚本会对每个 bitcode 依次运行四个 Saber 检查器，并将 TXT、Markdown、JSON 和 slice 文件集中写入同一目录：

```bash
source ./setup.sh
./run_saber_all_defects.sh \
  --output-dir /path/to/output \
  --no-bof \
  /path/to/input.bc
```

- `--output-dir`：必填，统一输出目录。
- `--no-bof`：可选，仅运行 Saber；省略时额外运行现有 BOF 检查。
- 可在命令末尾传入多个 `.bc` 文件。

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

1. 从该目录发现 Saber TXT 告警；
2. 按 TXT 主名读取同目录的 `*_report.json`；
3. 直接复用报告中的源码片段、轨迹、路径条件和求解信息；
4. 将 LLM 分类结果与语义扩增内容原子写回同一 JSON 报告。

## 主要代码

- `svf-llvm/tools/SABER/saber.cpp`：Saber 入口和报告参数
- `svf/lib/SABER/`：各 Saber 检查器及报告实现
- `svf-llvm/tools/GraphReader/`：语义查询服务
- `svf-llvm/tools/BOF/`、`svf/lib/BOF/`：缓冲区越界检测
- `run_saber_all_defects.sh`：统一输出目录运行脚本
