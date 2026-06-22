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
find_package(OpenSSL REQUIRED)

# ── ixwebsocket：系统包优先，回退 FetchContent ─────────────
# build.sh 可能已将 ixwebsocket 安装到 /usr/local，先搜这里
list(APPEND CMAKE_PREFIX_PATH "/usr/local")
find_package(ixwebsocket QUIET)
if(NOT ixwebsocket_FOUND)
    message(STATUS "ixwebsocket not found by find_package, trying FetchContent...")
    include(FetchContent)
    FetchContent_Declare(
        ixwebsocket
        GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
        GIT_TAG        master
        GIT_SHALLOW    TRUE
    )
    FetchContent_Populate(ixwebsocket)
    if(ixwebsocket_SOURCE_DIR)
        add_subdirectory(${ixwebsocket_SOURCE_DIR} ${ixwebsocket_BINARY_DIR})
        set(ixwebsocket_FOUND TRUE CACHE BOOL "" FORCE)
        message(STATUS "ixwebsocket built from source via FetchContent")
    else()
        message(FATAL_ERROR
            "ixwebsocket not found. Install it first:\n"
            "  Ubuntu/Debian: sudo apt install libixwebsocket-dev\n"
            "  Or build from source: https://github.com/machinezone/IXWebSocket"
        )
    endif()
endif()

# ── 共享链接依赖 ──────────────────────────
add_library(qbot_chen_deps INTERFACE)
target_link_libraries(qbot_chen_deps INTERFACE
    CURL::libcurl
    nlohmann_json::nlohmann_json
    Threads::Threads
    SQLite::SQLite3
    ixwebsocket::ixwebsocket
    OpenSSL::SSL
    OpenSSL::Crypto
)
if(WIN32)
    target_link_libraries(qbot_chen_deps INTERFACE bcrypt)
endif()

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

    # mcp (cpp-mmp 原版，头文件和源文件都在 src/mcp/ 下)
    src/mcp/mcp_sse_client.cpp
    src/mcp/mcp_tool.cpp
    src/mcp/mcp_message.cpp
    src/mcp/mcp_manager.cpp
)

# include 路径：项目根目录即可，cpp-mmp 头文件已在 src/mcp/ 下
target_include_directories(qbot_chen_obj PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(qbot_chen_obj PUBLIC qbot_chen_deps)

# ── 生产可执行文件 ─────────────────────────
add_executable(qbot_chen
    $<TARGET_OBJECTS:qbot_chen_obj>
    main.cpp
)
set_target_properties(qbot_chen PROPERTIES OUTPUT_NAME "qbot-chen")
target_link_libraries(qbot_chen PRIVATE qbot_chen_deps)
target_include_directories(qbot_chen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# ── 测试 ──────────────────────────────────
enable_testing()
add_executable(qbot_chen_test
    $<TARGET_OBJECTS:qbot_chen_obj>
    test_cases.cpp
)
target_link_libraries(qbot_chen_test PRIVATE qbot_chen_deps)
target_include_directories(qbot_chen_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME qbot_chen_unit_tests COMMAND qbot_chen_test)

# ── 编译选项 ──────────────────────────────
# CMAKE_BUILD_TYPE 控制优化等级，此处只加告警选项
target_compile_options(qbot_chen_obj PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /utf-8>
)
target_compile_definitions(qbot_chen_obj PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:NOMINMAX>
    $<$<CXX_COMPILER_ID:MSVC>:WIN32_LEAN_AND_MEAN>
    $<$<CXX_COMPILER_ID:MSVC>:_CRT_DECLARE_NONSTDC_NAMES=1>
    $<$<CXX_COMPILER_ID:MSVC>:CPPHTTPLIB_OPENSSL_SUPPORT>
    $<$<CXX_COMPILER_ID:MSVC>:MCP_SSL>
)

