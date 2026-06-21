#include "src/core/base.h"
#include "src/core/config.h"
#include "src/bot/agent.h"
#include "src/bot/napcat_bot.h"
#include <curl/curl.h>

int main() {
    CURLcode globalRes = curl_global_init(CURL_GLOBAL_ALL);
    if (globalRes != CURLE_OK) {
        std::cerr << CLR_RED "[✗] curl_global_init failed: " << curl_easy_strerror(globalRes) << CLR_RESET << std::endl;
        return 1;
    }
    Config::get().load("config.json");   // 启动时加载配置，失败则使用内置默认值
    /*SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);*/
    std::cout << CLR_BOLD CLR_CYAN
        << "╔══════════════════════════════════════╗\n"
        << "║        chenshuzhe'S Toolkit          ║\n"
        << "╠══════════════════════════════════════╣\n"
        << "║  [1] Agent 模式 (Deepseek 对话)      ║\n"
        << "║  [2] NapCat 模式 (QQ 机器人)         ║\n"
        << "╚══════════════════════════════════════╝\n"
        << CLR_RESET;
    std::cout << "请选择模式: ";

    std::string choice;
    std::getline(std::cin, choice);

    if (choice == "2") {
        try {
            Napcat napcat;
            napcat.run();
        } catch (const std::exception& e) {
            std::cerr << CLR_RED "[✗] 致命错误: " << e.what() << CLR_RESET << std::endl;
        }
    } else {
        try {
            agent* p = new agent;
            p->run();
            delete p;
        } catch (const std::exception& e) {
            std::cerr << CLR_RED "[✗] 致命错误: " << e.what() << CLR_RESET << std::endl;
        }
    }

#ifdef _WIN32
    system("pause");
#endif
    curl_global_cleanup();
    return 0;
}