# SVFmemplus Interleaving 交互接口设计草案

## 目标
- 参照 interSA 的交互式模式，给 SVFmemplus 增加“分阶段 + JSON 行协议”的 interleaving 接口。
- 重点在基础建模数据阶段可插入交互，允许外部工具在分析继续前补充或修正模型。
- 复用现有 GraphReader 的查询能力，统一命令风格与响应结构。

## interSA 逻辑要点（参考）
- 交互分阶段：`STAGE_POST_INIT` 与 `STAGE_POST_REPORT`，每阶段进入后发送 `{"ready":true,...}`。
- JSON 行协议：每行一个 JSON 对象，支持命令、响应和错误。
- 阶段内允许注册自定义 API、查询 sources/sinks、查询函数/路径等。
- `continue` 推进阶段，`exit` 退出。

参考文件：
- /Users/xunyc/Documents/MemoryLeak/interleaving/interSA/SABER_INTERACTIVE_USAGE.md
- /Users/xunyc/Documents/MemoryLeak/interleaving/interSA/svf-llvm/include/SaberInteractive.h
- /Users/xunyc/Documents/MemoryLeak/interleaving/interSA/svf-llvm/lib/SaberCommands.cpp

## SVFmemplus 现状
- `GraphReader` 已是长驻进程，启动后构建 PAG/ICFG/SVFG 并进入 JSON 交互循环。
- 现有命令覆盖函数查询、路径查询、SVFG 节点查询等，但没有“阶段”概念。
- 基础建模数据的生成过程（PAG/ICFG/SVFG 构建）目前不可被 interleaving。

相关文件：
- /Users/xunyc/Documents/MemoryLeak/interleaving/SVFmemplus/svf-llvm/tools/GraphReader/GraphReader.cpp
- /Users/xunyc/Documents/MemoryLeak/interleaving/SVFmemplus/svf-llvm/tools/GraphReader/GraphReaderUtil.h
- /Users/xunyc/Documents/MemoryLeak/interleaving/SVFmemplus/svf-llvm/tools/GraphReader/PathQuery.h
- /Users/xunyc/Documents/MemoryLeak/interleaving/SVFmemplus/svf-llvm/tools/GraphReader/FunctionQuery.h

## 拟议的交互阶段
| Stage | stage_name | 触发点 | 主要用途 |
| --- | --- | --- | --- |
| 0 | post-pag | SVFIR/PAG 构建完成 | 补充/修正“基础建模数据”与 API 模型 |
| 1 | post-andersen | Andersen + ICFG 构建完成 | 检查 points-to、call graph、ICFG 状态 |
| 2 | post-svfg | SVFG 构建完成 | 值流/路径/节点查询（现有 GraphReader 能力） |

阶段进入时发送：
- `{"ready":true,"stage":<n>,"stage_name":"<name>","message":"svf-stage-ready"}`

继续/退出：
- `{"command":"continue"}` 进入下一阶段
- `{"command":"exit"}` 退出

## 命令集合（建议对齐 interSA + GraphReader）
控制类：
- `continue` 继续下一阶段
- `exit` 退出
- `ping` 返回当前 stage 与构建状态

建模数据相关（重点）：
- `add-custom-api` 对齐 interSA（alloc/free/fopen/fclose），要求在 post-pag 阶段生效
- `load-model-spec` 从 JSON 载入自定义模型（函数语义、源/汇、字段抽象规则）
- `list-model-spec` 输出当前已加载模型条目摘要
- `add-source-by-location`、`add-sink-by-location` 为后续分析准备

查询类（可直接复用 GraphReader）：
- `find-function-body-by-name`、`find-function-body-by-location`
- `find-all-function-call-sites`、`find-all-function-callees`
- `find-lvalue-key_svfgnode`、`find-formal_arg-key_svfgnode`、`find-actual_arg-key_svfgnode`
- `list-formal-arg-nodes`、`list-callsite-actual-arg-nodes`、`find-callsite-return-node`
- `list-svfg-nodes-by-location`、`get-svfg-node-info`
- `find-gep-cl`、`find-store-cl`、`show-code-line`

响应与错误格式：
- 统一为 `{"error":true,"message":"..."}` 与 `{"message":"..."}` 风格
- 与 interSA 的 JSON 结构保持兼容

## TODO（按优先级）
- P0: 在 GraphReader pipeline 中引入阶段控制与 `continue` 机制，使构建过程可 interleaving。
- P0: 设计并实现 `load-model-spec` 的最小 JSON schema（函数语义、alloc/free/fopen/fclose、sources/sinks）。
- P0: 复用 interSA 的 `add-custom-api` 命令与 `SaberCheckerAPI` 绑定。
- P1: 抽象一个 `SVFInteractive`（仿 `SaberInteractive`）用于 GraphReader 统一命令注册与阶段等待。
- P1: 将 GraphReader 现有命令注册化，避免 `if-else` 链扩展成本。
- P1: 将 GraphReader 现有初始化消息升级为 `stage-ready` 消息，并补齐 `ping`。
- P1: 让 `post-pag` 阶段支持“基础建模数据”输出快照（PAG 节点统计、类型分布、函数模型数量）。
- P1: 统一错误码与响应字段，兼容 interSA 工具链。
- P2: 处理同一行多次调用同名函数的定位歧义（GraphReaderUtil::selectCallICFGNode 的 TODO）。
- P2: 实现 GEP 栈追踪与 `find-*` 命令的 offsets 扩展（GraphReader.cpp 中的 TODO）。
- P2: 将 ICFG 不可达返回位置也纳入路径枚举，减少误判（GraphReader.cpp 中的 TODO）。
- P2: 增加最小回归样例，覆盖 stage 跳转 + 基础建模数据修改 + 查询链路。

## 兼容性与落地建议
- 先在 GraphReader 上验证协议与命令，再考虑迁入 SVFmemplus 的其他分析器。
- 复用 interSA 的 stage 消息格式，便于前端/脚本共用。
- 建模数据必须在 `post-pag` 阶段落地，否则会影响后续 Andersen/SVFG。
