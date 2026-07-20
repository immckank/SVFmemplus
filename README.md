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
`alerts/bof/<sha256>.json` 单告警文件。

每条告警输出为独立 JSON。除 leak 外，`content.path` 是一条裁剪后的 SVFG
值流 witness；leak 的 `content.paths` 是可能安全释放对象的路径，
`content.leak_condition` 表示安全条件并集的补集。

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
alerts/leak/<sha256>.json
alerts/dfree/<sha256>.json
alerts/uaf/<sha256>.json
alerts/uninit/<sha256>.json
```

`-report-dir` 默认值为当前目录。终端内容仅作为运行日志，不是下游输入。

## 统一运行（全局管线）

在仓库根目录从 `workflow.ini.example` 创建项目配置，其中 `bitcode_path` 直接指向
一个 `.bc` 文件：

```bash
python3 -m orchestrator --config workflow.ini analyze
python3 -m orchestrator --config workflow.ini analyze \
  --checkers leak,dfree
```

旧 `script/config.env` 用户仍可通过仓库根 `script/run_svf.sh` 调用兼容包装。

## 项目语义库

Saber 和 BOF 共享最小 `semantic-fact/v2` 项目语义库。语义库只有
`base_api`、`safe_alloc`、`safe_free`、`value_range` 和
`source_filter` 五个 scope；每个 scope 只有描述和 fact 列表，fact
不含 ID、状态、版本或时间字段。

```bash
saber -leak \
  -semantic-facts=/path/to/semantic_facts.json \
  -report-dir=/path/to/output \
  input.bc

bof -semantic-facts=/path/to/semantic_facts.json \
  -report-dir=/path/to/output input.bc
```

分析产物由 orchestrator 与仓库内上一次警报按 `alert_id` 做集合差分。
本次消失的旧警报继续保留并设置 `suppressed: true`；后续再次出现时恢复为
`suppressed: false`。抑制状态为 true 时 `score` 固定为 0。

## 与 FPhandler 联动

将 Saber 的输出目录配置为 FPhandler 的 `OUTPUT_DIR`。FPhandler 会：

1. 按 `defect_types` 从 `alerts/` 读取对应类型的单警报 JSON；
2. 直接使用 `content` 中的路径、源码上下文和 checker 证据；
3. 将结论追加到同一文件的 `classifications[]`。

## 主动学习（ActiveLearning）

SVFmemplus 已默认适配 ActiveLearning 的数据与警报格式，**无需为 Saber / BOF 新增命令行参数**。
告警写入时由 `UnifiedAlertWriter` 创建最小 Warning；图导出由独立工具 `svf-al-export` 完成，闭环编排见仓库根 `script/run_active_learning_loop.sh`。

### Warning 外壳

产出阶段未执行的字段保持 `null`，不做兼容补齐：

```json
{
  "alert_id": "sha256:...",
  "producer": "svfmemplus",
  "type": "uaf",
  "content": {
    "path": [{"role": "use", "location": {"file": "src/a.c", "line": 42}}],
    "evidence": {"checker": {"report_kind": "local_ordered"}}
  },
  "graph_ids": null,
  "suppressed": false,
  "classifications": null,
  "active_learning": null,
  "score": 0.5
}
```

`alert_id` 是规范化 `{producer,type,content}` 的 SHA-256，文件名为其摘要部分。
分类结论首次写入时将 `classifications` 变为数组并持续追加。
`graph_ids` 在图关联阶段结束后为空数组或实际图 ID，图 ID 形如 `heap:<svf-object-id>`。

### 警报 → 模型输入

`ActiveLearning/alerts.py` 承担警报到排序/推理参数的转换：读取顶层 `graph_ids` 与预测分数，写回最新 `active_learning={weight,model}` 并重算 `score`。
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
- 仓库根 `orchestrator/`：分析、警报对账和主动学习的统一服务与 CLI
- 仓库根 `script/run_svf.sh`：旧 `config.env` 的兼容入口
