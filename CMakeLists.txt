cmake_minimum_required(VERSION 3.14)
project(qbot_chen LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(IX_WEBSOCKET_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(IX_WEBSOCKET_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)

# ── 依赖查找 ──────────────────────────────
find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)

include(FetchContent)
FetchContent_Declare(
    ixwebsocket
    GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(ixwebsocket)

# ── 共享链接依赖 ──────────────────────────
add_library(qbot_chen_deps INTERFACE)
target_link_libraries(qbot_chen_deps INTERFACE
    CURL::libcurl
    nlohmann_json::nlohmann_json
    Threads::Threads
    SQLite::SQLite3
    ixwebsocket::ixwebsocket
)

# ── 共享代码（OBJECT 库） ──────────────────
add_library(qbot_chen_obj OBJECT
    # core
    src/core/config.cpp
    src/core/tools.cpp
    src/core/command.cpp

    # bot
    src/bot/agent.cpp
    src/bot/napcat_bot.cpp
    src/bot/deepseek.cpp
    src/bot/models.cpp
    src/bot/image_ocr_service.cpp
    src/bot/connection_pool.cpp

    # memory
    src/memory/long_memory.cpp
    src/memory/memory.cpp
    src/memory/memory_manager.cpp
    src/memory/jargon_miner.cpp
    src/memory/jargon_data_model.cpp
    src/memory/jargon_inference.cpp
    src/memory/vocabulary_manager.cpp
    src/memory/style_cache.cpp
    src/memory/style_learner.cpp

    # knowledge
    src/knowledge/knowledge_retriever.cpp
    src/knowledge/knowledge_sharing.cpp
    src/knowledge/quality_scorer.cpp
    src/knowledge/embedding_service.cpp
    src/knowledge/embedding_store.cpp
    src/knowledge/query_cache.cpp

    # plugin
    src/plugin/plugin_base.cpp
    src/plugin/plugin_loader.cpp
    src/plugin/builtin_plugins.cpp
    src/plugin/process_manager.cpp
    src/plugin/persistence.cpp
    src/plugin/embedding_plugin.cpp
    src/plugin/knowledge_base_plugin.cpp
    src/plugin/command_handler_plugin.cpp
    src/plugin/long_term_memory_plugin.cpp
    src/plugin/user_management_plugin.cpp

    # infra
    src/infra/logger.cpp
    src/infra/event_bus.cpp
    src/infra/database_migrator.cpp
    src/infra/cleanup_manager.cpp
    src/infra/message_deduplicator.cpp
    src/infra/perf_monitor.cpp
    src/infra/reply_optimizer.cpp

    # mcp
    src/mcp/mcp_client.cpp
    src/mcp/mcp_manager.cpp
)

# include 路径：src/ 为根（与 #include "module/file.h" 对应）
target_include_directories(qbot_chen_obj PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(qbot_chen_obj PUBLIC qbot_chen_deps)

# ── 生产可执行文件 ─────────────────────────
add_executable(qbot_chen
    $<TARGET_OBJECTS:qbot_chen_obj>
    main.cpp
)
set_target_properties(qbot_chen PROPERTIES OUTPUT_NAME "qbot-chen")
target_link_libraries(qbot_chen PRIVATE qbot_chen_deps)

# ── 测试 ──────────────────────────────────
enable_testing()
add_executable(qbot_chen_test
    $<TARGET_OBJECTS:qbot_chen_obj>
    test_cases.cpp
)
target_link_libraries(qbot_chen_test PRIVATE qbot_chen_deps)
add_test(NAME qbot_chen_unit_tests COMMAND qbot_chen_test)

# ── 编译选项 ──────────────────────────────
target_compile_options(qbot_chen_obj PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -O2>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /O2 /utf-8>
)
target_compile_definitions(qbot_chen_obj PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:NOMINMAX>
    $<$<CXX_COMPILER_ID:MSVC>:WIN32_LEAN_AND_MEAN>
    $<$<CXX_COMPILER_ID:MSVC>:_CRT_DECLARE_NONSTDC_NAMES=1>
)
