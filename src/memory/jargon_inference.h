#pragma once
#include "src/core/base.h"
#include "src/memory/jargon_data_model.h"
#include "src/bot/deepseek.h"
#include <string>
#include <vector>

using json = nlohmann::json;

// 行话 LLM 推理引擎（3 层推理）
class JargonInference {
public:
    static JargonInference& get();

    // 执行完整的 3 层推理
    // 返回最终结果（已综合 inference1、inference2、comparison）
    InferenceResult infer(const JargonData& jargon, const std::vector<std::string>& recent_messages,
                         Deepseek* deepseek = nullptr, CURL* curl = nullptr);

private:
    JargonInference() = default;

    // 第 1 层：基于原始上下文推理
    InferenceResult infer_with_context(const std::string& content,
                                       const std::string& raw_content_text,
                                       const std::string& previous_meaning = "");

    // 第 2 层：基于词汇本身推理
    InferenceResult infer_content_only(const std::string& content);

    // 第 3 层：比较两个结果
    // 返回 is_jargon 字段表示两个推理结果是否差异大（有差异 = 真行话）
    InferenceResult compare_inferences(const InferenceResult& result1,
                                      const InferenceResult& result2);

    // 解析 JSON 格式的 LLM 响应
    InferenceResult parse_llm_response(const std::string& response);

    // 调用 LLM（代理到 deepseek）
    std::string call_llm(const std::string& prompt, Deepseek* deepseek = nullptr, CURL* curl = nullptr);

    // 成员变量存储 deepseek 和 curl
    Deepseek* _deepseek = nullptr;
    CURL* _curl = nullptr;
};
