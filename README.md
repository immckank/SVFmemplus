# SVFmem+

`SVFmemplus` 面向 LLVM bitcode（`.bc`）执行内存缺陷静态分析，并输出可供人工阅读和下游程序消费的增强告警报告。

## 功能

Saber 当前支持：

- `-leak`：内存泄漏（`NeverFree`、`PartialLeak`）
- `-dfree`：重复释放（`DoubleFree`）
- `-uaf`：释放后使用（`UseAfterFree`）
- `-uninit`：未初始化使用（`Uninitialized Use`）

`bof` 工具用于检测缓冲区越界（`BufferOverflow`），通过
`-report-dir=<dir>` 直接输出统一的
`alerts/buffer_overflow/<sha256>.json` 单告警文件。

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

1. 按 `defect_types` 从 `alerts/` 读取对应类别的单警报 JSON；
2. 直接使用警报 path、源码上下文和 checker 证据；
3. 将 `classification` 与 `reason` 原子写回同一文件。

## 主动学习（ActiveLearning）

SVFmemplus 已默认适配 ActiveLearning 的数据与警报格式，**无需为 Saber / BOF 新增命令行参数**。
告警写入时由 `UnifiedAlertWriter` 自动补齐 `active_learning` 与 `classifications` 字段；图导出由独立工具 `svf-al-export` 完成，闭环编排见仓库根 `script/run_active_learning_loop.sh`。

### 告警 JSON 扩展

在现有单警报 JSON 上增加 `active_learning` 对象（写告警时自动创建，读告警时缺字段也会补齐）：

```json
{
  "active_learning": {
    "schema_version": "active-learning/v1",
    "graph_ids": [],
    "match_status": "unresolved",
    "score": null,
    "rank": null,
    "last_model": null
  },
  "classifications": []
}
```

顶层 `classification` / `reason` 仍表示最新结论；多轮反馈历史保存在 `classifications[]` 中。
`graph_ids` 由下游排序阶段根据告警证据位置与 `graph_index.csv` 回填，图 ID 形如 `heap:<svf-object-id>`。

### 警报 → 模型输入

`ActiveLearning/alerts.py` 承担警报到排序/推理参数的转换：读取 `active_learning.graph_ids` 与预测分数，写回 `score`、`rank`、`match_status`。
该逻辑位于 ActiveLearning 模块内，后续接入其他静态分析器时在此扩展，而不改动 SVFmemplus 告警 JSON 外壳。

### 值流邻域图导出（`svf-al-export`）

对 CI（context-insensitive）指针分析中的每个 heap object，抽取其 SVFG 值流邻域子图，并按 ActiveLearning 约定的 CSV schema 写出：

```bash
svf-al-export --output-dir /path/to/output/active_learning/predict_dataset/raw/<stem> input.bc
```

产出文件：

```text
<output-dir>/
├── 0.node.csv
├── 0.edge.csv
├── …
├── idToGraph.csv      # 无表头，每行一个 graph ID
└── graph_index.csv    # graph_id,object_id,file,line,column,source_loc
```

邻域深度默认为 2 跳（含与 heap object 相关的 anchor 节点及其出入边可达节点）。
若某 heap object 在 SVFG 中无对应节点，仍输出仅含 anchor 节点的占位图，保证下游推理可识别该 object。

**节点 CSV**（`id,pattern,type,level,pointedBy`）与 **边 CSV**（`srcid,tgtid,type`）列含义见 `ActiveLearning/README.md`。

C/C++ 导出器中的 `type` / `edge.type` 整数枚举与模型 `edge_type_vocab_size=100` 对齐：

| edge.type | SVFG 边 |
| --- | --- |
| `0` | `IntraDirectVF` |
| `1` | `IntraIndirectVF` |
| `2` | `CallDirVF` |
| `3` | `RetDirVF` |
| `4` | `CallIndVF` |
| `5` | `RetIndVF` |
| `6` | `ThreadMHPIndirectVF` |
| `7..99` | 保留 |

节点 `type` 为粗粒度分组（`0..19`），如 `1=address/allocation`、`2=load`、`12=heap object anchor` 等，完整表见 `ActiveLearning/README.md`。

导出完成后，可用随机权重模型做一次 smoke test：

```bash
PYTHONPATH=ActiveLearning python3 -m cli predict \
  --dataset /path/to/predict_dataset \
  --output /path/to/predictions.csv \
  --random-weights
```

## 主要代码

- `svf-llvm/tools/SABER/saber.cpp`：Saber 入口和报告参数
- `svf/lib/SABER/`：各 Saber 检查器及报告实现
- `svf/lib/Util/UnifiedAlertWriter.cpp`：单警报 JSON 持久化与 `active_learning` 字段补齐
- `svf-llvm/tools/ActiveLearningExport/`：`svf-al-export`，CI heap object 值流邻域图导出
- `svf-llvm/tools/GraphReader/`：语义查询服务
- `svf-llvm/tools/BOF/`、`svf/lib/BOF/`：缓冲区越界检测
- 仓库根 `script/run_svf.sh`：全局管线静态分析入口
- 仓库根 `script/run_active_learning_loop.sh`：主动学习闭环（导出 → 推理 → 排序 → 反馈）
