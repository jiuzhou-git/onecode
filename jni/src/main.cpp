#include "imgui_image.h"
#include "picture/picture.h"
#include "Android_draw/draw.h"

#include <thread>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <linux/input.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/poll.h>
#include <sys/sysinfo.h>
#include <cstdlib>
#include <sys/types.h>
#include <sys/stat.h>
#include <csignal>
#include <cerrno>
#include <deque>
#include <mutex>

#if defined(USE_OPENGL)
    #include "imgui_image.h"
#else
    #include "VulkanUtils.h"
#endif

#if defined(USE_OPENGL)
    TextureInfo op_img;
#else
    MyTextureData vk_img;
#endif

// 日志系统全局变量
std::deque<std::string> logMessages;
std::mutex logMutex;
bool showLogWindow = false;
const size_t MAX_LOG_MESSAGES = 500;

// 全局变量
std::atomic<bool> isMainThreadRunning(true);
std::atomic<bool> showUI(true);

// 进程相关函数声明
static int SELECTEDPROCESSINDEX = -1;
static std::vector<std::pair<pid_t, std::string>> PROCESSLIST;
static std::mutex PROCESSLISTMUTEX;
static bool FILTERSYSTEMPROCESSES = false;
static bool PROCESS_WINDOW = false;
const ImVec2 PROCESSLISTBUTTONSIZE(200, 50);

// 帧率相关函数声明
bool FRAMERATESTATUS = false;
bool FRAMERATECOLORMODE = false;
ImVec4 FRAMERATECOLOR = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);

// 测试
static bool ISVERTICAL = true;
static float MAXVALUE = 640.0f;
static float CURRENTVALUE = 0.0f;

// 测试1
static bool TEST1 = false;
static float TEST1_X;
static float TEST1_Y;

// 使用常量代替重复计算
const ImVec2 BUTTONSIZE(150, 50);
ImVec4 BUTTONCOLOR = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);

// 输入设备管理结构
struct InputDevice {
    int fd;
    std::string path;
    bool valid;
};

// 添加日志消息
void AddLogMessage(const std::string& message) {
    std::lock_guard<std::mutex> lock(logMutex);
    logMessages.push_back(message);
    if (logMessages.size() > MAX_LOG_MESSAGES) {
        logMessages.pop_front();
    }
}

// 重定向cout到日志系统
class LogStream : public std::streambuf {
public:
    int overflow(int c) override {
        if (c != EOF) {
            char buf[2] = {static_cast<char>(c), 0};
            AddLogMessage(buf);
        }
        return c;
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        AddLogMessage(std::string(s, n));
        return n;
    }
};

// 将程序转为守护进程（Daemon）
void daemonize() {
    std::cout << "请选择执行方式:\n";
    std::cout << "1. 前台执行\n";
    std::cout << "2. 后台执行\n";

    int choice;
    std::cin >> choice;

    if (choice != 2) {
        return;
    }

    std::cout << "正在转为后台执行...\n";
    
    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "第一次 fork 失败: " << strerror(errno) << "\n";
        exit(EXIT_FAILURE);
    } else if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    if (setsid() < 0) {
        std::cerr << "setsid 失败: " << strerror(errno) << "\n";
        exit(EXIT_FAILURE);
    }

    pid = fork();
    if (pid < 0) {
        std::cerr << "第二次 fork 失败: " << strerror(errno) << "\n";
        exit(EXIT_FAILURE);
    } else if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    if (chdir("/") < 0) {
        std::cerr << "无法更改工作目录: " << strerror(errno) << "\n";
        exit(EXIT_FAILURE);
    }

    for (int fd = sysconf(_SC_OPEN_MAX); fd >= 0; --fd) {
        close(fd);
    }

    int null_fd = open("/dev/null", O_RDWR);
    if (null_fd < 0) {
        std::cerr << "无法打开 /dev/null: " << strerror(errno) << "\n";
        exit(EXIT_FAILURE);
    }
    dup2(null_fd, STDIN_FILENO);
    dup2(null_fd, STDOUT_FILENO);
    dup2(null_fd, STDERR_FILENO);
    close(null_fd);
}

// 音量键监听线程
void volumeKeyListener() {
    const std::string INPUT_DEVICE_DIR = "/dev/input";
    std::vector<InputDevice> devices;
    auto lastScanTime = std::chrono::steady_clock::now();
    const auto SCAN_INTERVAL = std::chrono::seconds(5);
    
    auto scanDevices = [&]() {
        DIR* dir = opendir(INPUT_DEVICE_DIR.c_str());
        if (!dir) return;
        
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_name[0] == '.' || 
                strncmp(entry->d_name, "event", 5) != 0) {
                continue;
            }
            
            std::string path = INPUT_DEVICE_DIR + "/" + entry->d_name;
            
            bool exists = false;
            for (auto& dev : devices) {
                if (dev.path == path) {
                    exists = true;
                    dev.valid = true;
                    break;
                }
            }
            if (exists) continue;
            
            int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
            if (fd < 0) continue;
            
            devices.push_back({fd, path, true});
        }
        closedir(dir);
    };
    
    scanDevices();
    
    while (isMainThreadRunning) {
        
        auto now = std::chrono::steady_clock::now();
        if (now - lastScanTime > SCAN_INTERVAL) {
            
            for (auto& dev : devices) {
                dev.valid = false;
            }
            
            scanDevices();
            
            for (auto it = devices.begin(); it != devices.end(); ) {
                if (!it->valid) {
                    close(it->fd);
                    it = devices.erase(it);
                } else {
                    ++it;
                }
            }
            
            lastScanTime = now;
        }
        
        std::vector<struct pollfd> fds;
        for (const auto& dev : devices) {
            fds.push_back({dev.fd, POLLIN, 0});
        }
        
        int ret = poll(fds.data(), fds.size(), 100);
        if (ret <= 0) continue;
        
        for (size_t i = 0; i < fds.size(); ++i) {
            if (!(fds[i].revents & POLLIN)) continue;
            
            struct input_event ev;
            while (read(fds[i].fd, &ev, sizeof(ev)) == sizeof(ev)) {
                if (ev.type != EV_KEY) continue;
                
                if (ev.code == KEY_VOLUMEUP && ev.value == 1) {
                    showUI = !showUI;
                }
                else if (ev.code == KEY_VOLUMEDOWN && ev.value == 1) {
                    showUI = !showUI;
                }
            }
            
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                devices[i].valid = false;
            }
        }
    }
    
    for (auto& dev : devices) {
        close(dev.fd);
    }
}






// 绘制颜色条
void DrawColorBar(float width, float height, float MAXVALUE, float CURRENTVALUE, bool ISVERTICAL) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    
    MAXVALUE = (MAXVALUE <= 0) ? 1.0f : MAXVALUE;
    CURRENTVALUE = ImClamp(CURRENTVALUE, 0.0f, MAXVALUE);
    
    float ratio = CURRENTVALUE / MAXVALUE;
    
    if (ISVERTICAL) {
        const float filledHeight = height * ratio;
        const float emptyHeight = height - filledHeight;
        
        if (filledHeight > 0) {
            draw_list->AddRectFilled(p, ImVec2(p.x + width, p.y + filledHeight), IM_COL32(255, 0, 0, 255));
        }
        
        if (emptyHeight > 0) {
            draw_list->AddRectFilled(ImVec2(p.x, p.y + filledHeight), ImVec2(p.x + width, p.y + height), IM_COL32(255, 255, 255, 255));
        }
        
        draw_list->AddRect(p, ImVec2(p.x + width, p.y + height), IM_COL32(0, 0, 0, 255));
    } else {
        const float filledWidth = width * ratio;
        const float emptyWidth = width - filledWidth;
        
        if (filledWidth > 0) {
            draw_list->AddRectFilled(p, ImVec2(p.x + filledWidth, p.y + height), IM_COL32(255, 0, 0, 255));
        }
        
        if (emptyWidth > 0) {
            draw_list->AddRectFilled(ImVec2(p.x + filledWidth, p.y), ImVec2(p.x + width, p.y + height), IM_COL32(255, 255, 255, 255));
        }
        
        draw_list->AddRect(p, ImVec2(p.x + width, p.y + height), IM_COL32(0, 0, 0, 255));
    }
    
    ImGui::Dummy(ImVec2(width, height));
}

// 刷新进程列表
void RefreshProcessList() {
    std::lock_guard<std::mutex> lock(PROCESSLISTMUTEX);
    PROCESSLIST.clear();
    
    DIR* proc_dir = opendir("/proc");
    if (!proc_dir) {
        AddLogMessage("无法打开/proc目录");
        return;
    }
    
    struct dirent* entry;
    while ((entry = readdir(proc_dir)) != nullptr) {
        if (entry->d_type == DT_DIR && isdigit(entry->d_name[0])) {
            pid_t pid = static_cast<pid_t>(atoi(entry->d_name));
            
            std::string cmdline_path = std::string("/proc/") + entry->d_name + "/cmdline";
            std::ifstream cmdline_file(cmdline_path);
            if (cmdline_file) {
                std::string cmdline;
                std::getline(cmdline_file, cmdline);
                
                std::replace(cmdline.begin(), cmdline.end(), '\0', ' ');
                
                if (FILTERSYSTEMPROCESSES) {
                    if (cmdline.empty() || cmdline[0] == '/' || 
                        cmdline.find("zygote") != std::string::npos ||
                        cmdline.find("system_server") != std::string::npos) {
                        continue;
                    }
                }
                if (!cmdline.empty()) {
                    PROCESSLIST.emplace_back(pid, cmdline);
                }
            }
        }
    }
    closedir(proc_dir);
    
    std::sort(PROCESSLIST.begin(), PROCESSLIST.end(), 
        [](const auto& a, const auto& b) { return a.second < b.second; });
    
    AddLogMessage("已刷新进程列表，共 " + std::to_string(PROCESSLIST.size()) + " 个进程");
}

// 渲染日志窗口
void renderLogWindow() {
    if (!showLogWindow) return;

    ImGui::SetNextWindowSize(ImVec2(800, 400), ImGuiCond_FirstUseEver);
    ImGui::Begin("日志窗口", nullptr);

    if (ImGui::Button("清除日志")) {
        std::lock_guard<std::mutex> lock(logMutex);
        logMessages.clear();
    }
    ImGui::SameLine();
    
    static bool autoScroll = true;
    ImGui::Checkbox("自动滚动", &autoScroll);

    ImGui::Separator();
    
    ImGui::BeginChild("LogScrollRegion", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);

    std::lock_guard<std::mutex> lock(logMutex);
    for (const auto& message : logMessages) {
        ImGui::TextUnformatted(message.c_str());
    }

    if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }

    ImGui::EndChild();
    ImGui::End();
}





// 渲染样式编辑器面
void renderStyleEditorface() {
    ImGui::Begin("Style Editor");
    ImGui::ShowStyleEditor();
    ImGui::End();
}

// 渲染测试1接口
void renderTest1Interface() {
    if (TEST1) {
        ImGui::PushStyleColor(ImGuiCol_Text, BUTTONCOLOR);
        if (ImGui::Button("启动", BUTTONSIZE)) {
            TEST1 = !TEST1;
        }
        ImGui::PopStyleColor();
    } else {
        if (ImGui::Button("启动", BUTTONSIZE)) {
            TEST1 = !TEST1;
        }
    }
    
    ImGui::SliderFloat("X", &TEST1_X, 0.0f, native_window_screen_x);
    ImGui::SliderFloat("Y", &TEST1_Y, 0.0f, native_window_screen_y);
    
    if (!TEST1) return;
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    draw_list->AddLine(ImVec2(TEST1_X , TEST1_Y), ImVec2(TEST1_X + 100, TEST1_Y + 100), IM_COL32(255, 0, 0, 255), 1.0f);
}

// 渲染测试接口
void renderTestInterface() {
    ImGui::SliderFloat("最大值", &MAXVALUE, 0.0f, 1000.0f);
    ImGui::SliderFloat("当前值", &CURRENTVALUE, 0.0f, MAXVALUE);
    ImGui::Checkbox("垂直方向", &ISVERTICAL);

    ImGui::Begin("cxt Window", nullptr, 
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | 
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoBackground);
    
    if (ISVERTICAL) {
        DrawColorBar(5.0f, 300.0f, MAXVALUE, CURRENTVALUE, ISVERTICAL);
    } else {
        DrawColorBar(300.0f, 5.0f, MAXVALUE, CURRENTVALUE, ISVERTICAL);
    }
    
    ImGui::End();
}

// 渲染帧率接口
void renderFrameRateInterface() {
    if (FRAMERATESTATUS) {
        ImGui::PushStyleColor(ImGuiCol_Text, BUTTONCOLOR);
        if (ImGui::Button("启动", BUTTONSIZE)) {
            FRAMERATESTATUS = !FRAMERATESTATUS;
        }
        ImGui::PopStyleColor();
    } else {
        if (ImGui::Button("启动", BUTTONSIZE)) {
            FRAMERATESTATUS = !FRAMERATESTATUS;
        }
    }
    
    if (FRAMERATESTATUS) {
        if (FRAMERATECOLORMODE) {
            ImGui::PushStyleColor(ImGuiCol_Text, BUTTONCOLOR);
            if (ImGui::Button("调色", BUTTONSIZE)) {
                FRAMERATECOLORMODE = !FRAMERATECOLORMODE;
            }
            ImGui::PopStyleColor();
        } else {
            if (ImGui::Button("调色", BUTTONSIZE)) {
                FRAMERATECOLORMODE = !FRAMERATECOLORMODE;
            }
        }
    }

    if (FRAMERATESTATUS && FRAMERATECOLORMODE) {
        ImGui::ColorEdit4("", (float*)&FRAMERATECOLOR);
    }
}

// 渲染内存接口
void renderMemoryInterface() {
    struct sysinfo info;
    if (sysinfo(&info) != 0) {
        ImGui::Text("Failed to get system info");
        ImGui::EndChild();
        return;
    }

    const double GB = 1024.0 * 1024.0 * 1024.0;
    double ramTotal = static_cast<double>(info.totalram) / GB;
    double ramAvailable = static_cast<double>(info.totalram - info.freeram) / GB;
    double swapTotal = static_cast<double>(info.totalswap) / GB;
    double swapUsed = static_cast<double>(info.totalswap - info.freeswap) / GB;

    ImGui::Text("RAM %.2f GB / %.2f GB", ramAvailable, ramTotal);
    ImGui::Text("SWAP %.2f GB / %.2f GB", swapUsed, swapTotal);
}

// 渲染进程接口
void renderprocessInterface() {
    ImGui::BeginGroup();
    if (ImGui::Button("刷新进程列表", PROCESSLISTBUTTONSIZE)) {
        std::thread(RefreshProcessList).detach();
    }
    ImGui::SameLine();
    
    if (FILTERSYSTEMPROCESSES) {
        ImGui::PushStyleColor(ImGuiCol_Text, BUTTONCOLOR);
        if (ImGui::Button("过滤系统进程", PROCESSLISTBUTTONSIZE)) {
            FILTERSYSTEMPROCESSES = !FILTERSYSTEMPROCESSES;
        }
        ImGui::PopStyleColor();
    } else {
        if (ImGui::Button("过滤系统进程", PROCESSLISTBUTTONSIZE)) {
            FILTERSYSTEMPROCESSES = !FILTERSYSTEMPROCESSES;
        }
    }
    ImGui::EndGroup();
    
    if (SELECTEDPROCESSINDEX >= 0 && SELECTEDPROCESSINDEX < static_cast<int>(PROCESSLIST.size())) {
        if (ImGui::Button("终止选中进程", PROCESSLISTBUTTONSIZE)) {
            pid_t pid = PROCESSLIST[SELECTEDPROCESSINDEX].first;
            std::string name = PROCESSLIST[SELECTEDPROCESSINDEX].second;
            
            if (kill(pid, SIGKILL) == 0) {
                AddLogMessage("成功终止进程: " + std::to_string(pid) + " (" + name + ")");
                std::thread(RefreshProcessList).detach();
            } else {
                AddLogMessage("终止进程失败 " + std::to_string(pid) + ": " + strerror(errno));
            }
        }
    } else {
        ImGui::BeginDisabled();
        ImGui::Button("终止选中进程", PROCESSLISTBUTTONSIZE);
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    
    if (PROCESS_WINDOW) {
        ImGui::PushStyleColor(ImGuiCol_Text, BUTTONCOLOR);
        if (ImGui::Button("启动", PROCESSLISTBUTTONSIZE)) {
            PROCESS_WINDOW = !PROCESS_WINDOW;
        }
        ImGui::PopStyleColor();
    } else {
        if (ImGui::Button("启动", PROCESSLISTBUTTONSIZE)) {
            PROCESS_WINDOW = !PROCESS_WINDOW;
        }
    }
    
    if (!PROCESS_WINDOW) return;
    ImGui::SetNextWindowSize(ImVec2(900, 600));
    ImGui::Begin("process", nullptr, ImGuiWindowFlags_NoTitleBar);
    ImGui::BeginChild("PROCESSLIST", ImVec2(0, 0), false);
    {
        std::lock_guard<std::mutex> lock(PROCESSLISTMUTEX);
        
        if (PROCESSLIST.empty()) {
            ImGui::Text("没有进程数据，请点击刷新");
        } else {
            ImGui::Text("当前显示: %d 个进程 (过滤系统进程: %s)", 
                static_cast<int>(PROCESSLIST.size()), 
                FILTERSYSTEMPROCESSES ? "开启" : "关闭");
            ImGui::Separator();
            
            for (size_t i = 0; i < PROCESSLIST.size(); ++i) {
                const auto& proc = PROCESSLIST[i];
                bool isSelected = (SELECTEDPROCESSINDEX == static_cast<int>(i));
                
                std::string label = "[" + std::to_string(proc.first) + "] " + proc.second;
                if (ImGui::Selectable(label.c_str(), isSelected)) {
                    SELECTEDPROCESSINDEX = static_cast<int>(i);
                }
                
                if (label.size() > 100) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("...");
                }
            }
        }
    }
    ImGui::EndChild();
    
    ImGui::End();
}

// 渲染主接口
void renderMainInterface() {
    if (ImGui::Button("结束音乐", BUTTONSIZE)) {
        std::vector<std::string> patterns = {
            "com.netease.cloudmusic",
            "com.miui.player"
        };
        
        std::vector<pid_t> matching_pids;
        DIR* proc_dir = opendir("/proc");
        if (proc_dir) {
            struct dirent* entry;
            while ((entry = readdir(proc_dir)) != nullptr) {
                if (entry->d_type == DT_DIR && isdigit(entry->d_name[0])) {
                    pid_t pid = static_cast<pid_t>(atoi(entry->d_name));
                    
                    std::string cmdline_path = std::string("/proc/") + entry->d_name + "/cmdline";
                    std::ifstream cmdline_file(cmdline_path);
                    if (cmdline_file) {
                        std::string cmdline;
                        std::getline(cmdline_file, cmdline);
                        
                        for (const auto& pattern : patterns) {
                            if (cmdline.find(pattern) != std::string::npos) {
                                matching_pids.push_back(pid);
                                break;
                            }
                        }
                    }
                }
            }
            closedir(proc_dir);
        }
        
        AddLogMessage("尝试终止音乐播放器进程...");
        for (pid_t pid : matching_pids) {
            if (kill(pid, SIGKILL) == 0) {
                AddLogMessage("成功终止进程: " + std::to_string(pid));
            } else {
                AddLogMessage("无法终止进程 " + std::to_string(pid) + ": " + strerror(errno));
            }
        }
        if (matching_pids.empty()) {
            AddLogMessage("未找到匹配的进程");
        }
    }
    
    if (showLogWindow) {
        ImGui::PushStyleColor(ImGuiCol_Text, BUTTONCOLOR);
        if (ImGui::Button("显示日志", BUTTONSIZE)) {
            showLogWindow = !showLogWindow;
        }
        ImGui::PopStyleColor();
    } else {
        if (ImGui::Button("显示日志", BUTTONSIZE)) {
            showLogWindow = !showLogWindow;
        }
    }
    
    if (ImGui::Button("退出", BUTTONSIZE)) {
        isMainThreadRunning = false;
    }
}





void renderUI() {
    if (showUI) {
        renderLogWindow();
        ImGui::StyleColorsClassic();
        ImGui::SetNextWindowSize(ImVec2(970, 0));
        ImGui::Begin("IMGUI", nullptr, ImGuiWindowFlags_NoTitleBar);
        
        ImGui::Columns(2, "mainColumns", true);
        
        ImGui::SetColumnWidth(0, 250.0f);
        ImGui::SetColumnWidth(1, 720.0f);
        
        #if defined(USE_OPENGL)
            ImGui::Image(
                (void*)(intptr_t)op_img.textureId,
                ImVec2(200, 200)
            );
        #else
            ImGui::Image(
                vk_img.DS,
                ImVec2(200, 200)
            );
        #endif
        ImGui::Text("九州出品");
        
        ImGui::NextColumn();
        
        if (ImGui::BeginTabBar("MainTabBar")) {
            if (ImGui::BeginTabItem("主界面")) {
                renderMainInterface();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("进程")) {
                renderprocessInterface();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("内存")) {
                renderMemoryInterface();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("帧率")) {
                renderFrameRateInterface();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("测试")) {
                renderTestInterface();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("测试1")) {
                renderTest1Interface();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Style")) {
                renderStyleEditorface();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        
        ImGui::Columns(1);
        
        ImGui::End();
    }

    if (FRAMERATESTATUS) {
        ImGui::Begin("Fixed Window", nullptr, 
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | 
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoBackground);
        
        ImGui::PushStyleColor(ImGuiCol_Text, FRAMERATECOLOR);
        ImGui::Text("%.1f", ImGui::GetIO().Framerate);
        ImGui::PopStyleColor();
        
        ImGui::End();
    }
    
    return;
}





int main(int argc, char *argv[]) {
    daemonize();
    
    // 获取屏幕信息
    screen_config(); 
    ::native_window_screen_x = displayInfo.width;
    // ::native_window_screen_x = displayInfo.height;
    ::native_window_screen_y = displayInfo.height;
    
    // 初始化imgui
    if (!initGUI_draw(native_window_screen_x, native_window_screen_y, true)) {
        return -1;
    }
    
    // 重定向cout到日志系统
    static LogStream logStream;
    std::cout.rdbuf(&logStream);
    
    AddLogMessage("当前程序Pid: " + std::to_string(getpid()));
    
    #if defined(USE_OPENGL)
        op_img = createTexture_ALL_FromMem(picture_jpg, sizeof(picture_jpg));
        if (op_img.textureId == 0) {
            std::cerr << "Failed to load OpenGL texture!" << std::endl;
        }
    #else
        if (!LoadTextureFromMemory((const void *)&picture_jpg, sizeof(picture_jpg), &vk_img)) {
            std::cerr << "Failed to load Vulkan texture!" << std::endl;
        }
    #endif
    
    Touch_Init(displayInfo.width, displayInfo.height, displayInfo.orientation, true);
    
    // 启动音量键监听线程
    std::thread volumeKeyThread(volumeKeyListener);

    ImGuiStyle& style = ImGui::GetStyle();
    style.ItemSpacing = ImVec2(25, 25);

    // 主循环
    while (isMainThreadRunning) {
        drawBegin();
        renderUI();
        drawEnd();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    volumeKeyThread.join();
    return 0;
}
