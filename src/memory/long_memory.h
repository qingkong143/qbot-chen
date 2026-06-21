#pragma once
#include "src/core/base.h"

struct LongMemoryContext {
    int64_t chat_id = 0;
    std::string chat_type;
    int64_t user_id = 0;
    std::string user_name;
    std::string user_message;
    std::string bot_reply;
};

class LongMemory {
public:
    static LongMemory& get();

    bool open(const std::string& path = "long_memory.db");
    void close();

    std::string buildPromptContext(const LongMemoryContext& ctx);
    void writeBackAfterReply(const LongMemoryContext& ctx);

    static std::string trim(const std::string& s);
    static std::string clip(const std::string& s, size_t maxLen);

private:
    LongMemory() = default;
    ~LongMemory() { close(); }

    void* _db = nullptr;

    void ensureSchema();
    std::string queryProfile(const LongMemoryContext& ctx, int maxChars);
    std::string queryFacts(const LongMemoryContext& ctx, int limit);
    std::string queryEpisodes(const LongMemoryContext& ctx, int limit);
    std::string hybridQueryFacts(const LongMemoryContext& ctx, int limit);
    std::vector<std::pair<int64_t, double>> keywordSearchFacts(const std::string& query, int limit);
    std::vector<std::pair<int64_t, double>> vectorSearchFacts(const std::string& query, int limit, class Models& models, CURL* curl);
    void insertRelationEdge(int64_t userId1, int64_t userId2, const std::string& relation);
    std::string queryGraphRelations(int64_t userId);
    void recordFeedback(int64_t factId, const std::string& feedback, const std::string& action);
    void upsertProfile(const LongMemoryContext& ctx);
    void insertFact(const LongMemoryContext& ctx, const std::string& fact, const std::string& category, double confidence);
    bool factExists(const LongMemoryContext& ctx, const std::string& fact) const;
    void insertEpisode(const LongMemoryContext& ctx, const std::string& title, const std::string& summary, double importance);
    void enqueueEmbedding(const std::string& memoryType, int64_t memoryId, const std::string& text);
    bool shouldRememberMessage(const std::string& text) const;
    std::string sanitizeMessageForMemory(const std::string& text) const;
    static double simpleTokenSimilarity(const std::string& a, const std::string& b);
};
