#pragma once
#include "src/core/base.h"
#include "src/bot/deepseek.h"

class Models {
public:
	Deepseek deepseek;

	// 调用 embedding API 获取文本向量
	std::vector<double> getEmbedding(CURL* curl, const std::string& text);
};