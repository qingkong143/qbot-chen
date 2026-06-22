#pragma once

#include<math.h>
#include<optional>
#include <string>
#include <filesystem>
#include <sstream>
#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <windows.h>
#else
    #include <unistd.h>
    #include <cstdlib>
#endif
#include <cwchar>
#include <iostream>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <system_error>
#include <vector>
#include <numeric> // for accumulate

// ── ANSI 终端颜色 ──────────────────────────────────
#define CLR_RESET   "\033[0m"
#define CLR_DIM     "\033[2m"
#define CLR_RED     "\033[31m"
#define CLR_GREEN   "\033[32m"
#define CLR_YELLOW  "\033[33m"
#define CLR_BLUE    "\033[34m"
#define CLR_MAGENTA "\033[35m"
#define CLR_CYAN    "\033[36m"
#define CLR_WHITE   "\033[37m"
#define CLR_BOLD    "\033[1m"
#define CLR_BG_BLK  "\033[40m"
