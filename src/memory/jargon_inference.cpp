#include "src/memory/jargon_inference.h"
#include "src/infra/logger.h"
#include "src/bot/deepseek.h"
#include <algorithm>
#include <sstream>

JargonInference& JargonInference::get() {
    static JargonInference instance;
    return instance;
}

InferenceResult JargonInference::infer(const JargonData& jargon,
                                       const std::vector<std::string>& recent_messages,
                                       Deepseek* deepseek, CURL* curl) {
    // 保存传入的模型和 curl 实例
    _deepseek = deepseek;
    _curl = curl;
    // 跳过手动创建的行话
    if (jargon.created_by == JargonCreatedBy::MANUAL) {
        Logger::get().debug("[行话推理]", "词汇 '" + jargon.content + "' 是手动记录，跳过推理");
        InferenceResult result;
        result.is_jargon = true;
        result.meaning = jargon.meaning;
        return result;
    }

    // 跳过已完成推断的行话
    if (jargon.is_complete) {
        Logger::get().debug("[行话推理]", "词汇 '" + jargon.content + "' 已完成推断，跳过");
        InferenceResult result;
        result.is_jargon = jargon.is_jargon;
        result.meaning = jargon.meaning;
        return result;
    }

    std::string raw_content_text;
    try {
        auto raw_arr = json::parse(jargon.raw_content);
        if (raw_arr.is_array()) {
            for (const auto& item : raw_arr) {
                raw_content_text += item.get<std::string>() + "\n";
            }
        }
    } catch (...) {
        raw_content_text = jargon.raw_content;
    }

    if (raw_content_text.empty()) {
        Logger::get().warn("[行话推理]", "词汇 '" + jargon.content + "' 无上下文，跳过推理");
        InferenceResult result;
        result.no_info = true;
        return result;
    }

    // 第 1 层：基于上下文推理
    Logger::get().info("[行话推理]", "词汇 '" + jargon.content + "' 开始第 1 层推理（with_context）");
    auto result1 = infer_with_context(jargon.content, raw_content_text, jargon.meaning);

    // 第 2 层：基于词汇本身推理（即使第 1 层信息不足，也要执行，以获得交叉验证）
    Logger::get().info("[行话推理]", "词汇 '" + jargon.content + "' 开始第 2 层推理（content_only）");
    auto result2 = infer_content_only(jargon.content);

    // 第 3 层：比较两个结果（如果第 1 层信息不足，通过差异性判断）
    Logger::get().info("[行话推理]", "词汇 '" + jargon.content + "' 开始第 3 层推理（compare）");
    auto final_result = compare_inferences(result1, result2);

    Logger::get().info("[行话推理]", "词汇 '" + jargon.content + "' 推理完成: is_jargon=" +
                      (final_result.is_jargon ? "true" : "false") + ", meaning=" + final_result.meaning);

    return final_result;
}

InferenceResult JargonInference::infer_with_context(const std::string& content,
                                                    const std::string& raw_content_text,
                                                    const std::string& previous_meaning) {
    std::string prompt =
        "你是网络文化和群聊用语分析专家。请分析以下词汇是否是群聊中的行话/黑话/梗。\n\n"
        "词汇: \"" + content + "\"\n"
        "出现上下文:\n" + raw_content_text + "\n";

    if (!previous_meaning.empty()) {
        prompt += "\n上一次推断的含义（仅供参考）:\n" + previous_meaning + "\n"
                 "请结合新的上下文，给出更准确或更新的推断结果。\n";
    }

    prompt += "\n请按 JSON 格式输出（只输出 JSON，不要其他内容）:\n"
             "{\n"
             "  \"is_jargon\": true/false/null,\n"
             "  \"meaning\": \"词汇含义或解释\",\n"
             "  \"no_info\": false,\n"
             "  \"confidence\": 0.0-1.0\n"
             "}\n\n"
             "判断标准:\n"
             "- true: 这是群内特有的行话、梗、黑话或网络用语（非标准词汇）\n"
             "- false: 这是标准词汇、常用词或通用网络术语\n"
             "- null: 无法判定，信息不足\n"
             "如果无法判定，设置 no_info: true";

    auto response = call_llm(prompt);
    return parse_llm_response(response);
}

InferenceResult JargonInference::infer_content_only(const std::string& content) {
    std::string prompt =
        "你是网络文化分析专家。仅根据词汇本身，判断 \"" + content + "\" 是否可能是网络行话/黑话/梗。\n\n"
        "请按 JSON 格式输出（只输出 JSON）:\n"
        "{\n"
        "  \"is_jargon\": true/false/null,\n"
        "  \"meaning\": \"可能的含义\",\n"
        "  \"confidence\": 0.0-1.0\n"
        "}\n\n"
        "注意：这是基于词汇形式的推断，可能不准确。";

    auto response = call_llm(prompt);
    return parse_llm_response(response);
}

InferenceResult JargonInference::compare_inferences(const InferenceResult& result1,
                                                   const InferenceResult& result2) {
    std::string prompt =
        "你是推理验证专家。请比较以下两个对同一词汇的推断结果，判断它们是否相似。\n\n"
        "结果 1（基于上下文）: " + result1.to_json().dump() + "\n"
        "结果 2（仅基于词汇）: " + result2.to_json().dump() + "\n\n"
        "请按 JSON 格式输出：\n"
        "{\n"
        "  \"is_similar\": true/false,\n"
        "  \"reason\": \"简短说明\",\n"
        "  \"confidence\": 0.0-1.0\n"
        "}\n\n"
        "判断规则：\n"
        "- is_similar=true: 两个结果一致（都说是行话 或 都说不是），说明这不是真行话\n"
        "- is_similar=false: 两个结果不一致（一个说是一个说否），说明这是真行话";

    auto response = call_llm(prompt);
    auto comparison = parse_llm_response(response);

    // 解释比较结果：
    // LLM 返回的 is_jargon 字段实际表示 is_similar
    // 相似(true) = 两结果一致 = 不是真行话(false)
    // 不相似(false) = 两结果差异 = 是真行话(true)
    InferenceResult final_result;
    bool is_similar = comparison.is_jargon;  // 重新命名以避免混淆
    final_result.is_jargon = !is_similar;    // 反转：相似 => 非行话
    final_result.meaning = result1.meaning;  // 始终使用第 1 层（基于上下文）的含义
    final_result.confidence = comparison.confidence;
    return final_result;
}

InferenceResult JargonInference::parse_llm_response(const std::string& response) {
    InferenceResult result;
    result.raw_response = response;

    try {
        auto j = json::parse(response);
        result.is_jargon = j.value("is_jargon", false);  // 默认改为 false（保守判定）
        result.meaning = j.value("meaning", "");
        result.confidence = j.value("confidence", 0.0);
        result.no_info = j.value("no_info", false);
    } catch (const std::exception& e) {
        Logger::get().warn("[行话推理]", "JSON 解析失败: " + std::string(e.what()));
        result.no_info = true;
        result.is_jargon = false;  // 异常时也显式设置为 false，避免误判为行话
    }

    return result;
}

std::string JargonInference::call_llm(const std::string& prompt, Deepseek* deepseek, CURL* curl) {
    // 代理到 deepseek 调用
    try {
        // 如果没有传入参数，使用成员变量
        if (!deepseek) deepseek = _deepseek;
        if (!curl) curl = _curl;

        // 如果仍然没有实例，返回空
        if (!deepseek || !curl) {
            Logger::get().warn("[行话推理]", "Deepseek 或 CURL 实例为空");
            return "";
        }

        json messages = json::array();
        messages.push_back({{"role", "user"}, {"content", prompt}});

        auto result = deepseek->SendChatCompletion(curl, messages, json::array());

        if (result.contains("choices") && !result["choices"].empty() &&
            result["choices"][0].contains("message") &&
            result["choices"][0]["message"].contains("content")) {
            return result["choices"][0]["message"]["content"].get<std::string>();
        }
    } catch (const std::exception& e) {
        Logger::get().error("[行话推理]", "LLM 调用失败: " + std::string(e.what()));
    }

    return "";
}
