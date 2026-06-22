# QBot — AI 驱动的 QQ 机器人

> 参考框架：[MaiBot](https://github.com/mai-bot/MaiBot)（架构设计）、[cpp-mmp](https://github.com/username/cpp-mmp)（MCP SSE 客户端实现）
>
> QQ 协议对接：[NapCat](https://github.com/NapNeko/NapCat)（OneBot WebSocket 协议）

基于 NapCat（OneBot 协议）和 DeepSeek 大模型构建的智能 QQ 机器人。支持群聊/私聊、长期记忆、语义检索、图片 OCR、行话挖掘、MCP 远程工具扩展等能力。

---

## 架构总览

```
┌─────────────────────────────────────────────────────────────────┐
│                        QQ 客户端（用户）                          │
└───────────────────────────┬─────────────────────────────────────┘
                            │ 消息事件
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│  NapCat (OneBot WebSocket)                                      │
│  ws://127.0.0.1:3001                                           │
└───────────────────────────┬─────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│  NapcatBot (napcat_bot.cpp)                                     │
│  ┌─────────────┐  ┌──────────────┐  ┌───────────────────────┐  │
│  │ 消息接收 &   │→│ 去重 & 冷却   │→│ @ / 未@ 分流          │  │
│  │  UTF-8 清洗  │  │ isAtSpamming │  │ TimingGate            │  │
│  └─────────────┘  └──────────────┘  └───────────┬───────────┘  │
│                                                   │              │
│  ┌────────────────────────────────────────────────┼───────────┐  │
│  │                                                ▼           │  │
│  │  ┌──────────────────┐    ┌──────────────────────────┐    │  │
│  │  │  Agent 实例       │    │  Agent 实例（私聊每人独立）│    │  │
│  │  │  （群聊共享）     │    │                          │    │  │
│  │  └────────┬─────────┘    └──────────┬───────────────┘    │  │
│  │           │                         │                      │  │
│  │           ▼                         ▼                      │  │
│  │  ┌─────────────────────────────────────────────────────┐  │  │
│  │  │  LLM 对话循环                                       │  │  │
│  │  │  1. 系统提示词 + 用户画像 + 群风格注入               │  │  │
│  │  │  2. 上下文压缩（超限时 summarize）                   │  │  │
│  │  │  3. LLM 回复 → Tool Call（可选）                    │  │  │
│  │  │  4. 工具执行：搜索/OCR/记忆/行话/命令                │  │  │
│  │  │  5. 结果回填 → 再次 LLM → 最终回复                  │  │  │
│  │  │  6. 历史压缩写入 & 长期记忆写回                      │  │  │
│  │  └─────────────────────────────────────────────────────┘  │  │
│  └───────────────────────────────────────────────────────────┘  │
│                                                                  │
│  ┌─────────────┐  ┌──────────────┐  ┌────────────────────────┐  │
│  │ Message      │  │ Knowledge    │  │ ImageOcrService        │  │
│  │ Deduplicator │  │ Retriever    │  │ (Agent toolcall 触发)  │  │
│  └─────────────┘  └──────────────┘  └────────────────────────┘  │
└──────────────────────────────────────────────────────────────────┘
```

---

## 核心流程

### 群聊消息处理

```
消息到达
  │
  ├─ isAtOnlyMessage() → 纯 @ 无内容 → 忽略
  ├─ isAtSpamming()    → 5秒内3次重复 → 拒绝
  │
  ├─ @ 到 Bot？
  │   ├─ 是 → 跳过冷却 → 直接进入 Agent 回复
  │   └─ 否 → 检查用户级冷却 → TimingGate 评估 → ReplyDecision
  │
  ├─ Agent 处理
  │   ├─ 加载长期记忆（AMemorix）
  │   ├─ 加载群聊风格（StyleCache）
  │   ├─ 构造 system prompt（含用户画像 + 风格规则）
  │   ├─ 上下文超限 → 压缩历史
  │   ├─ LLM 生成 → 可能触发 Tool Call
  │   │       ├─ search_history：查询本地 SQLite 历史
  │   │       ├─ MCP 远程工具（如 fetch）：通过 MCP Server 扩展能力
  │   │       ├─ embed / search_similar：语义检索
  │   │       └─ exec_cmd：管理员命令
  │   └─ 回复 → quickReplyLong（自动分段，UTF-8 安全）
  │
  └─ 异步：历史压缩 + 记忆写回 + 嵌入库更新
```

### 私聊消息处理

- 每个 QQ 用户有独立的 `agent` 实例
- 共享相同的 LLM 和工具配置
- 各自维护独立的历史上下文

---

## 核心模块

| 模块 | 文件 | 职责 |
|------|------|------|
| **NapcatBot** | `src/bot/napcat_bot.cpp/.h` | QQ 消息入口、冷却、去重、Agent 调度 |
| **Agent** | `src/bot/agent.cpp/.h` | LLM 对话循环、工具调用、上下文压缩 |
| **Tools** | `src/bot/tools.cpp/.h` | 工具注册/执行：OCR、命令、目录浏览、MCP 远程工具 |
| **Models** | `src/bot/models.cpp/.h` | LLM API 封装（DeepSeek 兼容 OpenAI 格式） |
| **Deepseek** | `src/bot/deepseek.cpp/.h` | HTTP 请求层（libcurl） |
| **ImageOcrService** | `src/bot/image_ocr_service.cpp/.h` | 图片 OCR（Agent toolcall 按需触发） |
| **ConnectionPool** | `src/bot/connection_pool.cpp/.h` | curl 连接池 |
| **Config** | `src/core/config.cpp/.h` | 配置加载 & 必填校验 |
| **Command** | `src/core/command.cpp/.h` | 命令处理 |
| **MCP Manager** | `src/mcp/mcp_manager.cpp/.h` | MCP 远程工具管理器（基于 cpp-mmp 的 SSE 客户端） |
| **cpp-mmp** | `src/mcp/mcp_sse_client.cpp/.h` 等 | MCP 协议 SSE 传输层（httplib + OpenSSL） |
| **EmbeddingService** | `src/knowledge/embedding_service.cpp/.h` | 向量 API 客户端（缓存 + 并发 + 重试） |
| **EmbeddingStore** | `src/knowledge/embedding_store.cpp/.h` | 向量持久化（SQLite per-namespace + 旧 JSON 迁移） |
| **KnowledgeRetriever** | `src/knowledge/knowledge_retriever.cpp/.h` | 知识库 CRUD |
| **QualityScorer** | `src/knowledge/quality_scorer.cpp/.h` | 知识质量评分 |
| **KnowledgeSharing** | `src/knowledge/knowledge_sharing.cpp/.h` | 知识共享 |
| **QueryCache** | `src/knowledge/query_cache.cpp/.h` | 查询缓存 |
| **LongMemory** | `src/memory/long_memory.cpp/.h` | 长期记忆 SQLite 存储 |
| **JargonMiner** | `src/memory/jargon_miner.cpp/.h` | 群聊行话自动挖掘 |
| **JargonDataModel** | `src/memory/jargon_data_model.cpp/.h` | 行话数据模型 |
| **JargonInference** | `src/memory/jargon_inference.cpp/.h` | 行话推理引擎 |
| **StyleCache** | `src/memory/style_cache.cpp/.h` | 群聊风格缓存 |
| **StyleLearner** | `src/memory/style_learner.cpp/.h` | 群聊风格学习 |
| **VocabularyManager** | `src/memory/vocabulary_manager.cpp/.h` | 词汇管理 |
| **Memory** | `src/memory/memory.cpp/.h` | 记忆 SQLite 底层操作 |
| **MemoryManager** | `src/memory/memory_manager.cpp/.h` | 记忆管理器 |
| **DatabaseMigrator** | `src/infra/database_migrator.cpp/.h` | 数据迁移 & 版本管理 |
| **MessageDeduplicator** | `src/infra/message_deduplicator.cpp/.h` | 消息去重（@ 前缀剥离） |
| **EventBus** | `src/infra/event_bus.cpp/.h` | 事件总线 |
| **CleanupManager** | `src/infra/cleanup_manager.cpp/.h` | 定时清理 |
| **PerfMonitor** | `src/infra/perf_monitor.cpp/.h` | 性能监控 |
| **ReplyOptimizer** | `src/infra/reply_optimizer.cpp/.h` | 回复优化 |
| **PluginLoader** | `src/plugin/plugin_loader.cpp/.h` | 插件加载器 |
| **PluginBase** | `src/plugin/plugin_base.cpp/.h` | 插件基类 |
| **BuiltinPlugins** | `src/plugin/builtin_plugins.cpp/.h` | 内置插件注册 |
| **ProcessManager** | `src/plugin/process_manager.cpp/.h` | 插件进程管理 |

---

## 数据存储

| 存储 | 文件 | 用途 |
|------|------|------|
| SQLite | `data/long_memory.db` | 长期记忆（用户画像、聊天摘要） |
| SQLite | `data/knowledge/*.db` | 知识库 |
| SQLite | `data/embedding/*.db` | 向量库（per-namespace） |
| SQLite | `data/jargons/*.db` | 行话数据 |
| JSON | `data/style_cache/` | 群风格缓存 |
| JSON | `data/a-memorix/` | AMemorix 记忆数据 |
| JSON | `config.json` | 用户配置（必须手动编辑，见 `config.example.json`） |

---

## 快速开始

### 1. 环境要求

- C++20 编译器（MSVC / GCC 10+ / Clang 12+）
- CMake 3.14+
- libcurl
- nlohmann/json
- SQLite3
- ixwebsocket

### 2. 构建

```bash
# Linux
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
# 或者直接
sh build.sh

# Windows (Visual Studio  Developer Command Prompt)
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022"
cmake --build . --config Release
```

### 3. 配置

首次运行会自动创建 `config.json` 模板，编辑后填入你的 API 信息：

```json
{
    "main_model": {
        "url": "https://api.deepseek.com/chat/completions",
        "api_key": "sk-xxx",
        "model": "deepseek-chat"
    },
    "summary_model": {
        "url": "https://api.deepseek.com/chat/completions",
        "api_key": "sk-xxx",
        "model": "deepseek-v4-flash"
    },
    "napcat": {
        "ws_url": "ws://127.0.0.1:3001",
        "http_url": "http://127.0.0.1:3000",
        "token": "your-napcat-token"
    }
}
```

必填字段：`main_model.url/api_key/model`、`summary_model.url/api_key/model`、`napcat.ws_url/http_url/token`。

可选字段（有默认值）：OCR、embedding、mcp_servers、pipeline、a_memorix、ban_words 等。

### 4. 运行

```bash
# 启动后选择模式
./qbot-chen
# [1] Agent 模式（控制台对话）
# [2] NapCat 模式（QQ 机器人）
```

---

## 关键设计决策

### 向量检索

- 内存线性扫描 + `partial_sort` 取 top-k，O(n log k)
- SQLite 做持久化（行级 upsert），自动迁移旧 JSON
- 缓存层减少重复 API 调用

### OCR 触发

- 不由消息到达时自动识别
- 由 LLM 通过 `recognize_image` toolcall 按需触发
- OCR 结果原文返回，不做 AI 摘要改写

### 消息去重

- `@` 前缀剥离后比较正文，避免不同消息因共同 @ 被误杀
- 纯 @ 无内容直接过滤
- 5 秒窗口内同一用户发送相同内容 ≥3 次判定为骚扰

### 上下文管理

- 超限时自动压缩历史（summary 模型）
- UTF-8 安全截断，避免坏字节导致 JSON 崩溃
- 长期记忆异步写回（AMemorix 模式）

---

## 目录结构

```
qbot-chen/
├── main.cpp                  # 入口，模式选择
├── main_system.cpp/.h        # 系统级入口（扩展模式）
├── test_cases.cpp            # 单元测试
├── test_framework.h          # 测试框架头
├── CMakeLists.txt            # 构建配置
├── build.sh                  # Linux 一键构建脚本
├── config.example.json       # 配置模板（复制为 config.json 后编辑）
├── src/
│   ├── bot/                  # QQ 机器人 & LLM 交互层
│   │   ├── napcat_bot.cpp/.h     # QQ 机器人主循环（消息接收、冷却、去重、Agent 调度）
│   │   ├── agent.cpp/.h          # LLM 对话循环、工具调用、上下文压缩
│   │   ├── tools.cpp/.h          # 工具注册/执行（搜索、OCR、命令）
│   │   ├── models.cpp/.h         # LLM API 封装（DeepSeek/OpenAI 兼容格式）
│   │   ├── deepseek.cpp/.h       # HTTP 请求层（libcurl）
│   │   ├── image_ocr_service.cpp/.h  # 图片 OCR（Agent toolcall 按需触发）
│   │   └── connection_pool.cpp/.h    # curl 连接池
│   │
│   ├── core/                 # 基础组件
│   │   ├── config.cpp/.h         # 配置加载 & 必填校验
│   │   ├── command.cpp/.h        # 命令处理
│   │   ├── tools.h/.cpp          # 工具定义头文件
│   │   ├── base.h                # 基础类型/宏
│   │   ├── math.h                # 数学工具
│   │   └── platform.h            # 平台抽象
│   │
│   ├── mcp/                  # MCP 远程工具扩展（基于 cpp-mmp）
│   │   ├── mcp_manager.cpp/.h    # MCP 工具管理器（URL 解析 + 工具注册）
│   │   ├── mcp_sse_client.cpp/.h # SSE 客户端（httplib 回调式 SSE）
│   │   ├── mcp_message.cpp/.h    # JSON-RPC 2.0 消息定义
│   │   ├── mcp_tool.cpp/.h       # MCP 工具定义
│   │   ├── mcp_logger.h          # 日志系统
│   │   ├── mcp_client.h          # 抽象基类
│   │   └── httplib.h             # HTTP 库（单头文件）
│   │
│   ├── knowledge/            # 知识库 & 向量检索
│   │   ├── embedding_service.cpp/.h  # 向量 API 客户端（缓存 + 并发 + 重试）
│   │   ├── embedding_store.cpp/.h    # 向量持久化（SQLite per-namespace + 旧 JSON 迁移）
│   │   ├── knowledge_retriever.cpp/.h # 知识库 CRUD
│   │   ├── quality_scorer.cpp/.h     # 知识质量评分
│   │   ├── knowledge_sharing.cpp/.h  # 知识共享
│   │   └── query_cache.cpp/.h        # 查询缓存
│   │
│   ├── memory/               # 记忆 & 行话
│   │   ├── long_memory.cpp/.h        # 长期记忆 SQLite 存储
│   │   ├── long_memory_phase3.cpp    # 长期记忆 Phase3 扩展
│   │   ├── memory.cpp/.h             # 记忆 SQLite 底层操作
│   │   ├── memory_manager.cpp/.h     # 记忆管理器
│   │   ├── jargon_miner.cpp/.h       # 群聊行话自动挖掘
│   │   ├── jargon_miner_old.cpp      # 旧版行话挖掘（保留参考）
│   │   ├── jargon_data_model.cpp/.h  # 行话数据模型
│   │   ├── jargon_inference.cpp/.h   # 行话推理引擎
│   │   ├── style_cache.cpp/.h        # 群聊风格缓存
│   │   ├── style_learner.cpp/.h      # 群聊风格学习
│   │   └── vocabulary_manager.cpp/.h # 词汇管理
│   │
│   ├── infra/                # 基础设施
│   │   ├── database_migrator.cpp/.h  # 数据迁移 & 版本管理
│   │   ├── message_deduplicator.cpp/.h # 消息去重（@ 前缀剥离）
│   │   ├── event_bus.cpp/.h          # 事件总线
│   │   ├── cleanup_manager.cpp/.h    # 定时清理
│   │   ├── perf_monitor.cpp/.h       # 性能监控
│   │   ├── reply_optimizer.cpp/.h    # 回复优化
│   │   └── logger.cpp/.h             # 日志系统
│   │
│   └── plugin/               # 插件系统
│       ├── plugin_loader.cpp/.h      # 插件加载器
│       ├── plugin_base.cpp/.h        # 插件基类
│       ├── builtin_plugins.cpp/.h    # 内置插件注册
│       ├── process_manager.cpp/.h    # 插件进程管理
│       ├── persistence.cpp/.h        # 插件持久化
│       ├── command_handler_plugin.cpp/.h  # 命令处理插件
│       ├── embedding_plugin.cpp/.h       # 嵌入向量插件
│       ├── knowledge_base_plugin.cpp/.h  # 知识库插件
│       ├── long_term_memory_plugin.cpp/.h # 长期记忆插件
│       └── user_management_plugin.cpp/.h  # 用户管理插件
│
└── data/                     # 运行时数据（首次运行自动创建）
    ├── embedding/            # 向量库（per-namespace SQLite）
    ├── knowledge/            # 知识库（SQLite）
    ├── jargons/              # 行话数据（SQLite）
    ├── long_memory.db        # 长期记忆（用户画像、聊天摘要）
    ├── style_cache/          # 群风格缓存（JSON）
    └── a-memorix/            # AMemorix 记忆数据
```

---

## 依赖关系

```
src/bot/napcat_bot ──→ src/bot/agent ──→ src/bot/models ──→ src/bot/deepseek（libcurl）
                          ├── src/bot/tools（搜索、OCR、命令）
                          ├── src/memory/jargon_miner
                          ├── src/infra/message_deduplicator
                          └── src/knowledge/knowledge_retriever

src/bot/agent ──→ src/knowledge/embedding_service ──→ src/knowledge/embedding_store（SQLite）
src/bot/agent ──→ src/memory/long_memory（SQLite）
src/bot/agent ──→ src/memory/style_cache
src/bot/agent ──→ src/knowledge/query_cache
src/bot/agent ──→ src/mcp/mcp_manager ──→ src/mcp/mcp_sse_client（cpp-mmp）

src/bot/napcat_bot ──→ src/bot/image_ocr_service（libcurl）
src/bot/napcat_bot ──→ src/bot/connection_pool（libcurl）
src/bot/napcat_bot ──→ src/infra/database_migrator

src/core/config ──→ 所有模块（配置注入）
src/infra/event_bus ──→ 跨模块事件分发
src/plugin/ ──→ 插件化扩展层（独立于主流程）
```

---

## 开发说明

### 添加新的 Agent 工具

在 `src/bot/tools.cpp` 中实现工具函数，然后在 `src/bot/agent.cpp` 的 `setupTools()` 中注册：

```cpp
// tools.cpp 中定义
static std::string tool_my_feature(const json& args) {
    // 解析 args，执行逻辑
    return "结果";
}

// agent.cpp 的 setupTools() 中绑定
tools.registerTool("my_feature", tool_my_feature);
```

工具函数签名：`std::string(const json& args)`，返回值即为 LLM 看到的工具执行结果。

### 添加新的必填配置项

在 `src/core/config.cpp` 的 `validate_required()` 中添加验证逻辑。

### 添加新的插件

1. 继承 `src/plugin/plugin_base.h` 中的 `PluginBase`
2. 在 `src/plugin/builtin_plugins.cpp` 中注册
3. 插件通过 `ProcessManager` 管理生命周期
