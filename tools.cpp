#include"tools.h"
#include "config.h"
#include <chrono>
#include <ctime>
#include <iomanip>
#include <thread>
#include <limits>
#ifndef _WIN32
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace {
static bool isUtf8ContinuationByte(unsigned char c) {
    return (c & 0xC0) == 0x80;
}

static std::string sanitizeUtf8Text(const std::string& input) {
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
                    if (!isUtf8ContinuationByte(static_cast<unsigned char>(input[i + j]))) {
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

static void sanitizeJsonStringsRecursive(json& value) {
    if (value.is_string()) {
        value = sanitizeUtf8Text(value.get<std::string>());
        return;
    }
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            sanitizeJsonStringsRecursive(it.value());
        }
        return;
    }
    if (value.is_array()) {
        for (auto& item : value) {
            sanitizeJsonStringsRecursive(item);
        }
    }
}
}

Tools::Tools() {
}

void Tools::registerTool(const std::string& name, const json& definition, ToolHandler handler) {
    _externalTools[name] = { definition, handler };
}

// ── 工具定义 helper（拆分为独立函数以避免 MSVC 递归类型膨胀） ──
static json tool_ListDirectory() {
    return {
        {"type", "function"},
        {"function", {
            {"name", "ListDirectory"},
            {"description", "列出指定目录下的文件。最多返回500条，超出自动截断。请优先使用精确路径，避免递归遍历整个系统盘"},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"root_path", {
                        {"type", "string"},
                        {"description", "想要查询的文件夹绝对路径，如 C:\\Users\\xxx\\Documents"}
                    }},
                    {"recursive", {
                        {"type","boolean"},
                        {"description","是否递归子目录（注意：深度递归易触发500条上限截断，建议先非递归查看）"}
                    }}
                }},
                {"required", {"root_path","recursive"}}
            }}
        }}
    };
}

static json tool_exec_cmd() {
    return {
        {"type", "function"},
        {"function", {
            {"name", "exec_cmd"},
            {"description", "调用命令行进行指令操作"},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"cmd", {
                        {"type", "string"},
                        {"description", "cmd命令"}
                    }}
                }},
                {"required", {"cmd"}}
            }}
        }}
    };
}

static json tool_get_current_time() {
    return {
        {"type", "function"},
        {"function", {
            {"name", "get_current_time"},
            {"description", "获取当前系统日期和时间（含星期、时区）。用于需要实时时间戳的场景，避免依赖过时的训练数据时间"}
        }}
    };
}

static json tool_wait() {
    return {
        {"type", "function"},
        {"function", {
            {"name", "wait"},
            {"description", "暂停当前推理轮次，等待指定秒数后再继续处理"},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"seconds", {
                        {"type", "integer"},
                        {"description", "等待秒数（1-60）"}
                    }}
                }},
                {"required", {"seconds"}}
            }}
        }}
    };
}

static json tool_finish() {
    return {
        {"type", "function"},
        {"function", {
            {"name", "finish"},
            {"description", "主动结束当前推理循环，立即返回最终回复"},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"reason", {
                        {"type", "string"},
                        {"description", "结束原因（可选）"}
                    }}
                }}
            }}
        }}
    };
}

static json tool_matrix_transform() {
    json params;
    params["type"] = "object";
    json properties;

    json mat;
    mat["type"] = "array";
    mat["description"] = "二维矩阵，每行是一个数组";
    json items_inner;
    items_inner["type"] = "number";
    items_inner["description"] = "矩阵元素（可以是整数或浮点数）";
    json items_arr = json::array({items_inner});
    mat["items"] = items_arr;
    mat["minItems"] = 1;
    mat["maxItems"] = 100;
    properties["mat"] = mat;

    json op;
    op["type"] = "string";
    op["enum"] = json::array({"swap_rows", "multiply_row", "add_row_multiple"});
    op["description"] = "要执行的变换类型。swap_rows=交换两行; multiply_row=某行乘以常数; add_row_multiple=将source_row的scale倍加到target_row";
    properties["op"] = op;

    json source_row;
    source_row["type"] = "integer";
    source_row["description"] = "源行索引（从0开始）：在multiply_row中是被乘的行，在add_row_multiple中是提供倍数的行";
    properties["source_row"] = source_row;

    json target_row;
    target_row["type"] = "integer";
    target_row["description"] = "目标行索引（从0开始）：在swap_rows中是另一行，在add_row_multiple中是被加的行（结果写入此行）";
    properties["target_row"] = target_row;

    json scale;
    scale["type"] = "number";
    scale["description"] = "缩放因子。multiply_row: 源行乘以此值；add_row_multiple: 源行乘以此值后加到目标行";
    properties["scale"] = scale;

    json required = json::array({"mat", "op", "source_row", "target_row"});
    params["properties"] = properties;
    params["required"] = required;

    json tool_def;
    tool_def["type"] = "function";
    tool_def["function"]["name"] = "matrix_transform";
    tool_def["function"]["description"] = "对矩阵执行初等变换";
    tool_def["function"]["parameters"] = params;

    return tool_def;
}

json Tools::GetTools() const {
    json arr = json::array({
        tool_ListDirectory(),
        tool_exec_cmd(),
        tool_get_current_time(),
        tool_wait(),
        tool_finish(),
        tool_matrix_transform()
    });

    // 追加外部注册的工具（NapCat 等模块注入）
    for (const auto& [name, rt] : _externalTools)
        arr.push_back(rt.definition);

    return arr;
}

void Tools::ProcessToolCalls(const json& toolCalls, json& toolResponses) {
    for (const auto& tc : toolCalls) {
        std::string id = tc["id"];
        std::string name = tc["function"]["name"];
        std::string argsRaw = tc["function"]["arguments"].get<std::string>();

        // 容错解析
        json args;
        try {
            args = json::parse(argsRaw);
        } catch (const json::parse_error& e) {
            toolResponses.push_back({
                {"role", "tool"},
                {"tool_call_id", id},
                {"content", "参数 JSON 不合法: " + sanitizeUtf8Text(std::string(e.what())) +
                    "\n原始参数: " + sanitizeUtf8Text(argsRaw) +
                    "\n注意：JSON 中不能包含算术表达式（如 -1/2 应写为 -0.5），数字必须是字面量。"}
            });
            continue;
        }

        // 优先检查外部注册的工具（NapCat 等模块注入）
        std::string ex_result;
        auto extIt = _externalTools.find(name);
        if (extIt != _externalTools.end()) {
            try {
                ex_result = extIt->second.handler(args);
            } catch (const std::exception& e) {
                ex_result = std::string("[外部工具异常] ") + e.what();
            }
            toolResponses.push_back({
                {"role", "tool"},
                {"tool_call_id", id},
                {"content", sanitizeUtf8Text(ex_result)}
            });
            continue;
        }

        std::string result;
        if (name == "ListDirectory") {
            std::string root_path = args["root_path"];
            // 安全提取 recursive（兼容 LLM 返回字符串 "true"/"false"）
            bool recursive = false;
            const auto& r = args["recursive"];
            if (r.is_boolean()) {
                recursive = r.get<bool>();
            }
            else if (r.is_string()) {
                std::string v = r.get<std::string>();
                std::transform(v.begin(), v.end(), v.begin(), ::tolower);
                recursive = (v == "true" || v == "1" || v == "yes");
            }
            else if (r.is_number_integer()) {
                recursive = (r.get<int>() != 0);
            }
            result = ListDirectory(root_path, recursive);
        }
        else if (name == "exec_cmd") {
            std::string cmd = args["cmd"];
            result = exec_cmd(cmd);
        }
        else if (name == "get_current_time") {
            result = get_current_time();
        }
        else if (name == "wait") {
            int seconds = args.value("seconds", 5);
            if (seconds < 1) seconds = 1;
            if (seconds > 60) seconds = 60;
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            result = "已等待 " + std::to_string(seconds) + " 秒";
        }
        else if (name == "finish") {
            std::string reason = args.value("reason", "推理完成");
            result = "[推理结束] " + reason;
        }
        else if (name == "matrix_transform") {
            try {
                // 解析矩阵
                std::vector<std::vector<double>> matData;
                for (const auto& row : args["mat"]) {
                    std::vector<double> rowData;
                    for (const auto& val : row) {
                        rowData.push_back(val.get<double>());
                    }
                    matData.push_back(rowData);
                }
                Matrix<double> mat(matData);
                std::string op = args["op"];
                int source_row = args.value("source_row", 0);
                int target_row = args.value("target_row", 0);

                if (op == "swap_rows") {
                    mat.swapRows(source_row, target_row);
                }
                else if (op == "multiply_row") {
                    double scale = args.value("scale", 1.0);
                    mat.multiplyRow(source_row, scale);
                }
                else if (op == "add_row_multiple") {
                    double scale = args.value("scale", 1.0);
                    mat.addRowMultiple(source_row, target_row, scale);
                }
                else {
                    result = "未知的矩阵变换类型: " + op;
                }

                if (result.empty()) {
                    result = mat.toString();
                    // 奇异矩阵检测
                    if (mat.isSingular()) {
                        result += "\n⚠ 注意：当前矩阵是奇异矩阵（行列式≈0），可能不可逆或有无穷多解。";
                    }
                }
            }
            catch (const std::exception& e) {
                result = std::string("矩阵变换出错: ") + e.what();
            }
        }

        // 构造 tool 响应消息
        result = sanitizeUtf8Text(result);
        toolResponses.push_back({
            {"role", "tool"},
            {"tool_call_id", id},
            {"content", result}
            });
    }
}

std::wstring Tools::Utf8ToUtf16(const std::string& utf8) {
    if (utf8.empty()) return {};
#ifdef _WIN32
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    if (len <= 0) return {};
    std::wstring result(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &result[0], len);
    return result;
#else
    std::wstring result;
    for (unsigned char c : utf8) {
        result.push_back((wchar_t)c);
    }
    return result;
#endif
}


std::string Tools::Utf16ToUtf8(const std::wstring& utf16) {
    if (utf16.empty()) return {};
#ifdef _WIN32
    int len = WideCharToMultiByte(CP_UTF8, 0, utf16.c_str(), (int)utf16.size(), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string result(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, utf16.c_str(), (int)utf16.size(), &result[0], len, nullptr, nullptr);
    return result;
#else
    std::string result;
    for (wchar_t c : utf16) {
        result.push_back((char)c);
    }
    return result;
#endif
}


void Tools::TraverseDirectory(const std::wstring& dirPath, std::ostringstream& oss,
                              bool recursive, int& count, int maxEntries) {
    if (count >= maxEntries) return;

#ifdef _WIN32
    std::wstring searchPath = dirPath + L"\\*";
    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (count >= maxEntries) break;
        if (wcscmp(findData.cFileName, L".") == 0 || wcscmp(findData.cFileName, L"..") == 0)
            continue;

        std::wstring fullPath = dirPath + L"\\" + findData.cFileName;
        oss << Utf16ToUtf8(fullPath) << '\n';
        count++;

        if (recursive && (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            TraverseDirectory(fullPath, oss, true, count, maxEntries);
        }
    } while (FindNextFileW(hFind, &findData));

    FindClose(hFind);
#else
    std::string dirStr;
    for (wchar_t c : dirPath) dirStr.push_back((char)c);

    DIR* dir = opendir(dirStr.c_str());
    if (!dir) return;

    struct dirent* entry;
    while ((entry = readdir(dir)) && count < maxEntries) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        std::string fullPath = dirStr + "/" + entry->d_name;
        oss << fullPath << '\n';
        count++;

        if (recursive && entry->d_type == DT_DIR) {
            std::wstring wFullPath;
            for (char c : fullPath) wFullPath.push_back((wchar_t)c);
            TraverseDirectory(wFullPath, oss, true, count, maxEntries);
        }
    }
    closedir(dir);
#endif
}

std::string Tools::ListDirectory(const std::string& root_path, bool recursive) {
#ifdef _WIN32
    std::wstring wRoot = Utf8ToUtf16(root_path);
    if (wRoot.empty()) return "";

    DWORD attr = GetFileAttributesW(wRoot.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
        return "查询的文件夹不存在";
#else
    struct stat st;
    if (stat(root_path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
        return "查询的文件夹不存在";

    std::wstring wRoot;
    for (char c : root_path) wRoot.push_back((wchar_t)c);
#endif

    const int MAX_ENTRIES = 500;
    std::ostringstream oss;
    int count = 0;
    TraverseDirectory(wRoot, oss, recursive, count, MAX_ENTRIES);

    std::cout << "[+] Listed " << count << " entries";
    if (count >= MAX_ENTRIES) {
        oss << "\n...[已达到上限 " << MAX_ENTRIES << " 条，后续内容省略]\n";
        std::cout << " (truncated at " << MAX_ENTRIES << ")";
    }
    std::cout << std::endl;
    return oss.str();
}


size_t Tools::WriteCallback(void* contents, size_t size, size_t nmemb, std::string* output) {
    size_t totalSize = size * nmemb;
    output->append((char*)contents, totalSize);
    return totalSize;
}

std::string Tools::exec_cmd(const std::string& cmd) {
#ifdef _WIN32
    HANDLE hStdoutRd, hStdoutWr;
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, FALSE };
    if (!CreatePipe(&hStdoutRd, &hStdoutWr, &sa, 0))
        return "ERROR: CreatePipe failed";

    PROCESS_INFORMATION pi = { 0 };
    STARTUPINFOA si = { 0 };
    si.cb = sizeof(STARTUPINFOA);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hStdoutWr;
    si.hStdError = hStdoutWr;

    std::string wrapped = "cmd.exe /c " + cmd;
    std::vector<char> cmdline(wrapped.begin(), wrapped.end());
    cmdline.push_back('\0');
    BOOL success = CreateProcessA(NULL, cmdline.data(), NULL, NULL,
        TRUE, 0, NULL, NULL, &si, &pi);

    CloseHandle(hStdoutWr);

    if (!success) {
        CloseHandle(hStdoutRd);
        return "ERROR: CreateProcess failed";
    }

    DWORD waitResult = WaitForSingleObject(pi.hProcess, 20000);
    std::string result;
    if (waitResult == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        result = "ERROR: 命令执行超时(>20s)，已强制终止\n";
    }

    DWORD bytesRead;
    char buffer[4096];
    while (ReadFile(hStdoutRd, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
        buffer[bytesRead] = '\0';
        result += buffer;
    }

    CloseHandle(hStdoutRd);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return sanitizeUtf8Text(result);
#else
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "ERROR: popen failed";

    std::string result;
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }

    int status = pclose(pipe);
    if (status != 0) {
        result += "\n[命令执行返回非零状态码: " + std::to_string(status) + "]\n";
    }

    return sanitizeUtf8Text(result);
#endif
}

std::string Tools::get_current_time() {
    // 获取 UTC 时间戳
    auto now = std::chrono::system_clock::now();
    std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm utc_tm;
#ifdef _WIN32
    gmtime_s(&utc_tm, &now_time);
#else
    gmtime_r(&now_time, &utc_tm);
#endif

    // 获取本地时间
    std::tm local_tm;
#ifdef _WIN32
    localtime_s(&local_tm, &now_time);
    TIME_ZONE_INFORMATION tzi;
    GetTimeZoneInformation(&tzi);
    int utc_offset = -tzi.Bias / 60;
#else
    localtime_r(&now_time, &local_tm);
    time_t now_utc = time(nullptr);
    struct tm tm_utc;
    gmtime_r(&now_utc, &tm_utc);
    struct tm tm_local;
    localtime_r(&now_utc, &tm_local);
    int utc_offset = (tm_local.tm_hour - tm_utc.tm_hour) + ((tm_local.tm_yday - tm_utc.tm_yday) * 24);
#endif
    int bias_minutes;
#ifdef _WIN32
    bias_minutes = -tzi.Bias;  // Windows Bias 是 UTC = Local + Bias (分钟)，取反得到 UTC 偏移
#else
    bias_minutes = utc_offset * 60;
#endif
    int offset_hours = bias_minutes / 60;

    // 获取毫秒
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    int milliseconds = static_cast<int>(ms.count());

    // 星期名称
    const char* weekdays[] = { "周日", "周一", "周二", "周三", "周四", "周五", "周六" };

    // ISO 8601 格式: 2026-06-06T14:30:25.123+08:00
    char buf[128];
    snprintf(buf, sizeof(buf),
        "%04d-%02d-%02dT%02d:%02d:%02d.%03d%+03d:00 %s",
        local_tm.tm_year + 1900,
        local_tm.tm_mon + 1,
        local_tm.tm_mday,
        local_tm.tm_hour,
        local_tm.tm_min,
        local_tm.tm_sec,
        milliseconds,
        offset_hours,
        weekdays[local_tm.tm_wday]);

    // 附加可读描述
    std::ostringstream oss;
    oss << buf << "\n"
        << "日期: " << (local_tm.tm_year + 1900) << "年"
        << (local_tm.tm_mon + 1) << "月"
        << local_tm.tm_mday << "日 "
        << weekdays[local_tm.tm_wday] << "\n"
        << "时间: " << std::setfill('0') << std::setw(2) << local_tm.tm_hour << ":"
        << std::setw(2) << local_tm.tm_min << ":"
        << std::setw(2) << local_tm.tm_sec << "."
        << std::setw(3) << milliseconds << "\n"
        << "时区: UTC" << (offset_hours >= 0 ? "+" : "") << offset_hours << ":00\n"
        << "Unix时间戳: " << now_time << " (秒)\n"
        << "星期: " << weekdays[local_tm.tm_wday] << " (day " << local_tm.tm_wday << ")\n"
        << "年份: " << (local_tm.tm_year + 1900) << " | 月份: " << (local_tm.tm_mon + 1)
        << " | 日期: " << local_tm.tm_mday;

    std::cout << "[+] Current time: " << buf << std::endl;
    return oss.str();
}
