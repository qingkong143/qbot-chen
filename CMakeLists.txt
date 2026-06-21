cmake_minimum_required(VERSION 3.14)
project(qbot_chen LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(IX_WEBSOCKET_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)  # 不编译测试
set(IX_WEBSOCKET_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)     # 不编译示例

# ── 依赖查找 ──────────────────────────────
find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)

# ixwebsocket：直接 FetchContent，不依赖系统包
include(FetchContent)
FetchContent_Declare(
    ixwebsocket
    GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(ixwebsocket)

# ── 共享链接依赖（INTERFACE 库，同时供主程序和测试链接） ──
add_library(qbot_chen_deps INTERFACE)
target_link_libraries(qbot_chen_deps INTERFACE
    CURL::libcurl
    nlohmann_json::nlohmann_json
    Threads::Threads
    SQLite::SQLite3
    ixwebsocket::ixwebsocket
)

# ── 共享代码（OBJECT 库，同时供主程序和测试链接） ──
add_library(qbot_chen_obj OBJECT
    agent.cpp
    jargon_miner.cpp
    cleanup_manager.cpp
    command.cpp
    config.cpp
    connection_pool.cpp
    database_migrator.cpp
    deepseek.cpp
    embedding_service.cpp
    image_ocr_service.cpp
    embedding_store.cpp
    event_bus.cpp
    jargon_data_model.cpp
    jargon_inference.cpp
    knowledge_retriever.cpp
    knowledge_sharing.cpp
    logger.cpp
    long_memory.cpp
    memory.cpp
    memory_manager.cpp
    message_deduplicator.cpp
    models.cpp
    mcp_client.cpp
    mcp_manager.cpp
    napcat_bot.cpp
    perf_monitor.cpp
    plugin_loader.cpp
    query_cache.cpp
    quality_scorer.cpp
    reply_optimizer.cpp
    style_cache.cpp
    style_learner.cpp
    vocabulary_manager.cpp
    tools.cpp

    # P0/P1/P2 框架
    plugin_base.cpp
    process_manager.cpp
    persistence.cpp
    embedding_plugin.cpp
    knowledge_base_plugin.cpp
    command_handler_plugin.cpp
    long_term_memory_plugin.cpp
    user_management_plugin.cpp
    builtin_plugins.cpp
    main_system.cpp
)

target_include_directories(qbot_chen_obj PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
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
