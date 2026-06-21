#include "test_framework.h"
#include "src/memory/long_memory.h"
#include "src/memory/style_learner.h"
#include "src/knowledge/query_cache.h"
#include "src/knowledge/embedding_store.h"
#include "src/bot/napcat_bot.h"
#include <filesystem>
#include <string>

namespace {
std::string sanitizeUtf8ForTest(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size();) {
        unsigned char c = static_cast<unsigned char>(input[i]);
        size_t extra = 0;
        bool valid = false;
        if ((c & 0x80) == 0x00) {
            extra = 0;
            valid = true;
        } else if ((c & 0xE0) == 0xC0) {
            extra = 1;
            valid = (c >= 0xC2);
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            valid = true;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            valid = (c <= 0xF4);
        }
        if (valid) {
            if (i + extra >= input.size()) {
                valid = false;
            } else {
                for (size_t j = 1; j <= extra; ++j) {
                    if ((static_cast<unsigned char>(input[i + j]) & 0xC0) != 0x80) {
                        valid = false;
                        break;
                    }
                }
            }
        }
        if (valid) {
            out.append(input, i, extra + 1);
            i += extra + 1;
        } else {
            out.push_back('?');
            ++i;
        }
    }
    return out;
}
}

class LongMemoryTest : public TestCase {
public:
    LongMemoryTest() : TestCase("LongMemory::trim") {}
    void test() override {
        std::string result = LongMemory::trim("  hello world  ");
        assert_equal("hello world", result);
    }
};

class ClipTest : public TestCase {
public:
    ClipTest() : TestCase("LongMemory::clip") {}
    void test() override {
        std::string result = LongMemory::clip("hello world", 5);
        assert_true(result.size() <= 8, "clipped string too long");
    }
};

class QueryCacheTest : public TestCase {
public:
    QueryCacheTest() : TestCase("QueryCache::basic") {}
    void test() override {
        QueryCache& cache = QueryCache::get();
        cache.cacheProfile(123, 456, "test_profile");
        std::string cached = cache.queryProfile(123, 456);
        assert_equal("test_profile", cached);
    }
};

class StyleLearnerTest : public TestCase {
public:
    StyleLearnerTest() : TestCase("StyleLearner::recordMessage") {}
    void test() override {
        StyleLearner& learner = StyleLearner::get();
        learner.recordMessageStyle(1, "这是一条测试消息", "user1");
        std::string insights = learner.getRecentTrendInsights(1);
        assert_not_empty(insights, "insights should not be empty");
    }
};

class EmbeddingStoreAutoPersistTest : public TestCase {
public:
    EmbeddingStoreAutoPersistTest() : TestCase("EmbeddingStore::auto_persist_after_first_insert") {}

    void test() override {
        namespace fs = std::filesystem;

        fs::path tempDir = fs::temp_directory_path() / "embedding_store_autopersist_test";
        fs::remove_all(tempDir);
        fs::create_directories(tempDir);

        EmbeddingManager::get().initialize(tempDir.string());
        auto& store = EmbeddingManager::get().get_store("knowledge");
        store.batch_insert_strs({"一条用于持久化验证的文本"});

        assert_true(fs::exists(tempDir / "knowledge.db"), "embedding db should exist right after first insert");

        fs::remove_all(tempDir);
    }
};

class EmbeddingStoreDebounceSaveTest : public TestCase {
public:
    EmbeddingStoreDebounceSaveTest() : TestCase("EmbeddingStore::debounced_save_request") {}

    void test() override {
        namespace fs = std::filesystem;
        fs::path tempDir = fs::temp_directory_path() / "embedding_store_debounce_test";
        fs::remove_all(tempDir);
        fs::create_directories(tempDir);

        EmbeddingStore store("knowledge", tempDir.string());
        store.request_save();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        store.flush_pending_saves();
        assert_true(fs::exists(tempDir / "knowledge.db"), "pending save should eventually create db file");

        fs::remove_all(tempDir);
    }
};

class Utf8SanitizeTest : public TestCase {
public:
    Utf8SanitizeTest() : TestCase("Utf8::sanitize_incomplete_tail") {}

    void test() override {
        std::string input = std::string("abc") + static_cast<char>(0xEF);
        std::string output = sanitizeUtf8ForTest(input);
        assert_equal(std::string("abc?"), output);
    }
};

class AtSpamDedupTest : public TestCase {
public:
    AtSpamDedupTest() : TestCase("MessageDeduplicator::at_prefix_not_overblocked") {}

    void test() override {
        MessageDeduplicator dedup;
        std::string first = "[CQ:at,qq=3482500590] 气死我了";
        std::string second = "[CQ:at,qq=3482500590] 怎么不理我";

        assert_true(dedup.shouldProcess(first, "1001"), "first at message should pass");
        dedup.addMessage(first, "1001");
        assert_true(dedup.shouldProcess(second, "1001"), "different at message should not be blocked as duplicate");
    }
};

class OcrToolResultTest : public TestCase {
public:
    OcrToolResultTest() : TestCase("Agent::recognize_image_not_summarized") {}

    void test() override {
        std::string raw = "价格信息\n在线推理 批量推理 微调训练\n这是OCR原文，不能被改写成摘要。";
        std::string safe = sanitizeUtf8ForTest(raw);
        assert_true(safe.find("OCR原文") != std::string::npos, "ocr text should be preserved after utf8 sanitization");
        assert_true(safe.find("[AI摘要]") == std::string::npos, "ocr tool should not be rewritten here");
    }
};

class EmbeddingStoreRestartPersistenceTest : public TestCase {
public:
    EmbeddingStoreRestartPersistenceTest() : TestCase("EmbeddingStore::restart_persistence") {}

    void test() override {
        namespace fs = std::filesystem;
        fs::path tempDir = fs::temp_directory_path() / "embedding_store_restart_test";
        fs::remove_all(tempDir);
        fs::create_directories(tempDir);

        {
            EmbeddingStore store("knowledge", tempDir.string());
            store.batch_insert_strs({"重启后仍应可读的文本"});
            store.save_to_file();
        }

        EmbeddingStore reopened("knowledge", tempDir.string());
        reopened.load_from_file();
        assert_true(reopened.size() > 0, "store should reload persisted rows after restart");
        assert_true(fs::exists(tempDir / "knowledge.db"), "knowledge db should exist after restart persistence");

        fs::remove_all(tempDir);
    }
};



namespace {
struct TestRegistrar {
    TestRegistrar() {
        auto& suite = TestSuite::get();
        suite.addTest(new LongMemoryTest());
        suite.addTest(new ClipTest());
        suite.addTest(new QueryCacheTest());
        suite.addTest(new StyleLearnerTest());
        suite.addTest(new EmbeddingStoreAutoPersistTest());
        suite.addTest(new EmbeddingStoreDebounceSaveTest());
        suite.addTest(new Utf8SanitizeTest());
        suite.addTest(new AtSpamDedupTest());
        suite.addTest(new OcrToolResultTest());
        suite.addTest(new EmbeddingStoreRestartPersistenceTest());
    }
};

TestRegistrar registrar;
}

TestSuite& TestSuite::get() {
    static TestSuite instance;
    return instance;
}

int main() {
    return TestSuite::get().runAll();
}
