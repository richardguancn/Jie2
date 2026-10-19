#include "platform/linux/wayland_portal.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <dbus/dbus.h>

namespace jie2::platform {

namespace {

// 门户坐标（xdg-desktop-portal 规范固定值）。
constexpr const char* kPortalBusName = "org.freedesktop.portal.Desktop";
constexpr const char* kPortalObjectPath = "/org/freedesktop/portal/desktop";
constexpr const char* kGlobalShortcutsIface =
    "org.freedesktop.portal.GlobalShortcuts";
constexpr const char* kRequestIface = "org.freedesktop.portal.Request";
constexpr const char* kSessionIface = "org.freedesktop.portal.Session";

// 超时：方法调用（建立请求）用短超时；等 Response（可能弹系统确认框）用长超时。
constexpr int kCallTimeoutMs = 5000;
constexpr int kResponseTimeoutMs = 12000;

/// 把修饰键渲染成门户约定的触发串（"CTRL+ALT+t"）。
/// 规范参考：xdg-desktop-portal GlobalShortcuts 的 preferred_trigger。
std::string portalTriggerString(const HotkeySpec& spec) {
    std::string out;
    if (spec.modifiers & kModCtrl) {
        out += "CTRL+";
    }
    if (spec.modifiers & kModAlt) {
        out += "ALT+";
    }
    if (spec.modifiers & kModShift) {
        out += "SHIFT+";
    }
    if (spec.modifiers & kModSuper) {
        out += "LOGO+";
    }
    // 键名：字母小写、数字原样、功能键小写（"f1"）。
    switch (spec.key) {
        case Key::A: case Key::B: case Key::C: case Key::D: case Key::E:
        case Key::F: case Key::G: case Key::H: case Key::I: case Key::J:
        case Key::K: case Key::L: case Key::M: case Key::N: case Key::O:
        case Key::P: case Key::Q: case Key::R: case Key::S: case Key::T:
        case Key::U: case Key::V: case Key::W: case Key::X: case Key::Y:
        case Key::Z: {
            const std::string full = describeHotkey(spec);
            // describeHotkey 输出 "CTRL+ALT+T" 形态；取最后一个字符小写。
            if (!full.empty()) {
                out += static_cast<char>(
                    std::tolower(static_cast<unsigned char>(full.back())));
            }
            break;
        }
        case Key::Num0: out += "0"; break;
        case Key::Num1: out += "1"; break;
        case Key::Num2: out += "2"; break;
        case Key::Num3: out += "3"; break;
        case Key::Num4: out += "4"; break;
        case Key::Num5: out += "5"; break;
        case Key::Num6: out += "6"; break;
        case Key::Num7: out += "7"; break;
        case Key::Num8: out += "8"; break;
        case Key::Num9: out += "9"; break;
        case Key::F1: out += "f1"; break;
        case Key::F2: out += "f2"; break;
        case Key::F3: out += "f3"; break;
        case Key::F4: out += "f4"; break;
        case Key::F5: out += "f5"; break;
        case Key::F6: out += "f6"; break;
        case Key::F7: out += "f7"; break;
        case Key::F8: out += "f8"; break;
        case Key::F9: out += "f9"; break;
        case Key::F10: out += "f10"; break;
        case Key::F11: out += "f11"; break;
        case Key::F12: out += "f12"; break;
        case Key::Space: out += "space"; break;
        case Key::Tab: out += "Tab"; break;
        case Key::Return: out += "Return"; break;
        case Key::Escape: out += "Escape"; break;
        case Key::Backspace: out += "BackSpace"; break;
        case Key::Delete: out += "Delete"; break;
        case Key::Insert: out += "Insert"; break;
        case Key::Home: out += "Home"; break;
        case Key::End: out += "End"; break;
        case Key::Left: out += "Left"; break;
        case Key::Right: out += "Right"; break;
        case Key::Up: out += "Up"; break;
        case Key::Down: out += "Down"; break;
        case Key::PageUp: out += "Page_Up"; break;
        case Key::PageDown: out += "Page_Down"; break;
        case Key::Minus: out += "minus"; break;
        case Key::Equal: out += "equal"; break;
        case Key::BracketLeft: out += "bracketleft"; break;
        case Key::BracketRight: out += "bracketright"; break;
        case Key::Backslash: out += "backslash"; break;
        case Key::Semicolon: out += "semicolon"; break;
        case Key::Apostrophe: out += "apostrophe"; break;
        case Key::Comma: out += "comma"; break;
        case Key::Period: out += "period"; break;
        case Key::Slash: out += "slash"; break;
        case Key::Grave: out += "grave"; break;
        case Key::F13: out += "f13"; break;
        case Key::F14: out += "f14"; break;
        case Key::F15: out += "f15"; break;
        case Key::F16: out += "f16"; break;
        case Key::F17: out += "f17"; break;
        case Key::F18: out += "f18"; break;
        case Key::F19: out += "f19"; break;
        case Key::F20: out += "f20"; break;
        case Key::Unknown: break;
    }
    return out;
}

/// 打开一个 {sv} 字典条目：key → 字符串 variant。
void appendStringVariant(DBusMessageIter* dict, const char* key,
                         const std::string& value) {
    DBusMessageIter entry;
    DBusMessageIter var;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &var);
    const char* raw = value.c_str();
    dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &raw);
    dbus_message_iter_close_container(&entry, &var);
    dbus_message_iter_close_container(dict, &entry);
}

/// 解析响应里的 a{sv}，只收集字符串与对象路径（我们关心的字段：
/// session_handle；其余字段跳过）。
void collectStringVariants(DBusMessageIter* arrayIter,
                           std::map<std::string, std::string>* out) {
    DBusMessageIter array;
    dbus_message_iter_recurse(arrayIter, &array);
    while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry;
        dbus_message_iter_recurse(&array, &entry);
        const char* key = nullptr;
        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
            dbus_message_iter_get_basic(&entry, &key);
        }
        dbus_message_iter_next(&entry);
        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
            DBusMessageIter var;
            dbus_message_iter_recurse(&entry, &var);
            const int type = dbus_message_iter_get_arg_type(&var);
            if (key != nullptr &&
                (type == DBUS_TYPE_STRING || type == DBUS_TYPE_OBJECT_PATH)) {
                const char* value = nullptr;
                dbus_message_iter_get_basic(&var, &value);
                if (value != nullptr) {
                    (*out)[key] = value;
                }
            }
        }
        dbus_message_iter_next(&array);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct WaylandPortalHotkeys::Impl {
    DBusConnection* conn = nullptr;
    bool filterInstalled = false;

    std::thread worker;
    std::atomic<bool> running{false};

    // ---- 单线程 dispatch 的任务队列 ----
    //
    // 为什么必须这样（实测教训）：
    //   libdbus 的同一连接**不允许被多个线程同时操作**。早期版本让调用
    //   线程执行 send_with_reply_and_block（内部会 poll+dispatch），同时
    //   worker 线程也在 read_write_dispatch —— 两个线程同时碰连接时，
    //   主线程的 pending-call 完成通知会丢失：实测 bind 的方法回复明明
    //   在 +218ms 就到达（filter 可见），主线程的 send_with_reply_and_block
    //   却阻塞到 +1684ms（下一个消息活动才"推醒"它），而 mock 的 1500ms
    //   Activated 触发器正好在那个窗口里发出 → 信号到达时回调表还没登记
    //   → 激活被静默丢弃（约 1/8 概率的偶发失败）。
    //
    // 现在的模型：
    //   主线程：只入队 + 等结果（不碰连接）；
    //   worker：唯一 dispatch 线程，顺序执行队列里的完整门户流程。
    struct Request {
        enum class Kind { Grab, ReleaseAll } kind = Kind::Grab;
        HotkeySpec spec;
        HotkeyCallback callback = nullptr;
        void* userData = nullptr;
        bool done = false;
        bool ok = false;
        std::string error;
    };
    std::mutex queueMutex;
    std::condition_variable queueCv;   // worker 等新任务
    std::condition_variable doneCv;    // 主线程等完成
    // 队列持有 shared_ptr：主线程等待超时提前返回时，worker 仍持有引用，
    // 不会出现悬空指针（超时路径的 use-after-free 是这里的经典坑）。
    std::vector<std::shared_ptr<Request>> queue;

    // 等 Response 的同步点（filter 在 worker 线程里写；
    // awaitResponse 在同一线程轮询读取 —— 因为等待期间它还要继续 dispatch）。
    std::mutex responseMutex;
    std::string expectedPath;      // 当前等待的 request 对象路径
    bool responseReady = false;
    dbus_uint32_t responseCode = 0;
    std::map<std::string, std::string> responseResults;

    /// 调试时间戳（相对起点毫秒），配合 JIE2_PORTAL_DEBUG 排查时序。
    const std::chrono::steady_clock::time_point startedAt =
        std::chrono::steady_clock::now();
    long long debugMs() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - startedAt)
            .count();
    }

    // 会话与绑定表。
    //
    // 协议约束（官方文档明确）："An application can only attempt to bind
    // shortcuts of a session once." —— 每个会话只能 BindShortcuts 一次。
    // 所以采用**每快捷键一个会话**的模型；releaseAll 时逐个关闭。
    std::mutex boundMutex;
    struct Bound {
        std::string sessionHandle;
        std::string id;
        HotkeyCallback callback = nullptr;
        void* userData = nullptr;
    };
    std::vector<Bound> bound;

    int tokenCounter = 0;

    ~Impl() { stop(); }

    // ---- 生命周期 ----

    bool ensureStarted(std::string* error) {
        if (conn != nullptr) {
            return true;
        }
        // 显式初始化 libdbus 线程支持（部分发行版构建要求）。
        dbus_threads_init_default();

        DBusError err;
        dbus_error_init(&err);
        conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
        if (conn == nullptr) {
            if (error != nullptr) {
                *error = std::string("无法连接会话总线（DBUS_SESSION_BUS_ADDRESS）：") +
                         (dbus_error_is_set(&err) ? err.message : "未知原因");
            }
            dbus_error_free(&err);
            return false;
        }
        dbus_connection_set_exit_on_disconnect(conn, FALSE);
        dbus_connection_add_filter(conn, &Impl::filterCallback, this, nullptr);
        filterInstalled = true;

        // 订阅两类信号：Response（请求结果）与 Activated（热键按下）。
        dbus_bus_add_match(
            conn,
            "type='signal',interface='org.freedesktop.portal.Request',"
            "member='Response'",
            &err);
        if (dbus_error_is_set(&err)) {
            dbus_error_free(&err);  // 匹配失败不致命（后续等待会超时并如实报错）
        }
        dbus_bus_add_match(
            conn,
            "type='signal',interface='org.freedesktop.portal.GlobalShortcuts',"
            "member='Activated'",
            &err);
        if (dbus_error_is_set(&err)) {
            dbus_error_free(&err);
        }
        dbus_connection_flush(conn);

        running.store(true);
        worker = std::thread([this] { workerLoop(); });
        return true;
    }

    void stop() {
        running.store(false);
        if (worker.joinable()) {
            worker.join();
        }
        if (conn != nullptr) {
            if (filterInstalled) {
                dbus_connection_remove_filter(conn, &Impl::filterCallback, this);
                filterInstalled = false;
            }
            dbus_connection_close(conn);
            dbus_connection_unref(conn);
            conn = nullptr;
        }
    }

    void workerLoop() {
        while (running.load()) {
            // ① 取任务（短等待，保证既不空转也不迟滞 dispatch）。
            std::shared_ptr<Request> request;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                queueCv.wait_for(lock, std::chrono::milliseconds(50), [this] {
                    return !queue.empty() || !running.load();
                });
                if (!queue.empty()) {
                    request = queue.front();
                    queue.erase(queue.begin());
                }
            }

            // ② 无任务：dispatch 一次（仍是唯一 dispatch 线程）。
            if (request == nullptr) {
                if (conn != nullptr) {
                    dbus_connection_read_write_dispatch(conn, 10);
                }
                continue;
            }

            // ③ 有任务：在 worker 线程完成整个门户流程
            //    （send_with_reply_and_block 也在这里调用 —— 单线程原则）。
            if (request->kind == Request::Kind::Grab) {
                request->ok = doGrabBlocking(request->spec, request->callback,
                                             request->userData, &request->error);
            } else {
                doReleaseAllBlocking();
                request->ok = true;
            }
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                request->done = true;
            }
            doneCv.notify_all();
        }
    }

    // ---- 消息过滤 ----

    static DBusHandlerResult filterCallback(DBusConnection*, DBusMessage* msg,
                                            void* data) {
        auto* self = static_cast<Impl*>(data);
        if (self != nullptr) {
            self->handleMessage(msg);
        }
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    void handleMessage(DBusMessage* msg) {
        // 排查开关：JIE2_PORTAL_DEBUG=1 时打印每条到达的消息
        // （含非信号消息？filter 在无匹配规则时只收到已订阅的信号与回复）。
        if (std::getenv("JIE2_PORTAL_DEBUG") != nullptr) {
            std::fprintf(stderr,
                         "[portal-debug +%lldms] msg type=%d iface=%s member=%s path=%s\n",
                         debugMs(), dbus_message_get_type(msg),
                         dbus_message_get_interface(msg) ? dbus_message_get_interface(msg) : "-",
                         dbus_message_get_member(msg) ? dbus_message_get_member(msg) : "-",
                         dbus_message_get_path(msg) ? dbus_message_get_path(msg) : "-");
        }
        if (dbus_message_is_signal(msg, kRequestIface, "Response")) {
            handleResponse(msg);
            return;
        }
        if (dbus_message_is_signal(msg, kGlobalShortcutsIface, "Activated")) {
            handleActivated(msg);
            return;
        }
    }

    void handleResponse(DBusMessage* msg) {
        const char* path = dbus_message_get_path(msg);
        DBusMessageIter it;
        if (!dbus_message_iter_init(msg, &it)) {
            return;
        }
        dbus_uint32_t code = 0;
        if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_UINT32) {
            return;
        }
        dbus_message_iter_get_basic(&it, &code);
        dbus_message_iter_next(&it);
        std::map<std::string, std::string> results;
        if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
            collectStringVariants(&it, &results);
        }

        {
            std::lock_guard<std::mutex> lock(responseMutex);
            if (expectedPath.empty() || path == nullptr ||
                expectedPath != path) {
                return;  // 不是当前等待的请求（例如上一个请求的迟到响应）
            }
            responseCode = code;
            responseResults = std::move(results);
            responseReady = true;
        }
        // 无需 notify：awaitResponse 在同一线程轮询检查 responseReady，
        // 且它的循环里带着 dispatch（见 awaitResponse 的注释）。
    }

    void handleActivated(DBusMessage* msg) {
        DBusMessageIter it;
        if (!dbus_message_iter_init(msg, &it)) {
            return;
        }
        // 参数：(o session_handle, s shortcut_id, t timestamp, a{sv} options)
        const char* session = nullptr;
        if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_OBJECT_PATH) {
            dbus_message_iter_get_basic(&it, &session);
        }
        dbus_message_iter_next(&it);
        const char* id = nullptr;
        if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRING) {
            dbus_message_iter_get_basic(&it, &id);
        }
        HotkeyCallback callback = nullptr;
        void* userData = nullptr;
        std::size_t boundCount = 0;
        {
            std::lock_guard<std::mutex> lock(boundMutex);
            boundCount = bound.size();
            if (id != nullptr) {
                for (const Bound& entry : bound) {
                    if (entry.id == id) {
                        callback = entry.callback;
                        userData = entry.userData;
                        break;
                    }
                }
            }
        }
        if (std::getenv("JIE2_PORTAL_DEBUG") != nullptr) {
            std::fprintf(stderr,
                         "[portal-debug +%lldms] Activated: id=%s bound=%zu matched=%d\n",
                         debugMs(), id != nullptr ? id : "(null)", boundCount,
                         callback != nullptr ? 1 : 0);
        }
        if (callback != nullptr) {
            // 回调在 D-Bus 线程触发（与 X11 后端同一约定）。
            callback(std::string(id), userData);
        }
    }

    // ---- 请求辅助 ----

    std::string nextToken(const char* purpose) {
        ++tokenCounter;
        return std::string("jie2_") + purpose + "_" +
               std::to_string(tokenCounter);
    }

    /// 计算 request 对象路径：/org/freedesktop/portal/desktop/request/<sender>/<token>
    /// sender 是唯一名（":1.42"）去掉冒号、点换下划线（"1_42"）。
    std::string makeRequestPath(const std::string& token) {
        std::string senderToken;
        const char* unique = dbus_bus_get_unique_name(conn);
        if (unique != nullptr) {
            for (const char* p = unique; *p != '\0'; ++p) {
                if (*p == ':') {
                    continue;
                }
                senderToken += (*p == '.') ? '_' : *p;
            }
        }
        return std::string("/org/freedesktop/portal/desktop/request/") +
               senderToken + "/" + token;
    }

    /// 预置等待状态：**必须在发请求之前调用**。
    ///
    /// 时序说明（这里有个真实的竞争，曾经写错过）：
    ///   portal 可能极快地回响应，filter 由 worker 线程执行 ——
    ///   如果先发请求再"开始等待"，一个快速的响应会在等待建立之前到达，
    ///   被 filter 记录后又被随后的等待初始化**清掉**，最终白等到超时。
    ///   正确顺序：先装好 expectedPath（锁定状态），再发请求，
    ///   最后纯等待（不再重置任何状态）。
    void prepareWait(const std::string& requestPath) {
        std::lock_guard<std::mutex> lock(responseMutex);
        responseReady = false;
        responseResults.clear();
        expectedPath = requestPath;
    }

    /// 等某个 request 的 Response（状态必须已由 prepareWait 预置）。
    ///
    /// **只在 worker 线程调用**，且等待期间**必须持续 dispatch**：
    /// worker 是唯一的 dispatch 线程，若它只是阻塞等待条件变量，
    /// 响应信号永远无人处理 —— 实测表现为每次都 12 秒超时
    /// （这是把模型改成单线程 dispatch 时引入的真实缺陷，别改回去）。
    bool awaitResponse(dbus_uint32_t* code,
                       std::map<std::string, std::string>* results,
                       int timeoutMs) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline && running.load()) {
            {
                std::lock_guard<std::mutex> lock(responseMutex);
                if (responseReady) {
                    *code = responseCode;
                    *results = std::move(responseResults);
                    responseReady = false;
                    expectedPath.clear();
                    return true;
                }
            }
            // 推进消息处理：filter 在 dispatch 中被调用，
            // 若收到匹配的 Response 会置 responseReady。
            if (conn != nullptr) {
                dbus_connection_read_write_dispatch(conn, 30);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        std::lock_guard<std::mutex> lock(responseMutex);
        expectedPath.clear();
        return false;
    }

    /// 发送方法调用并等回复对象路径（CreateSession / BindShortcuts 都返回 o）。
    bool callPortal(const char* method, DBusMessage* msg, std::string* error) {
        if (conn == nullptr) {
            *error = "D-Bus 连接未建立";
            dbus_message_unref(msg);
            return false;
        }
        DBusError err;
        dbus_error_init(&err);
        DBusMessage* reply = dbus_connection_send_with_reply_and_block(
            conn, msg, kCallTimeoutMs, &err);
        dbus_message_unref(msg);
        if (reply == nullptr) {
            *error = std::string(method) + " 调用失败：" +
                     (dbus_error_is_set(&err) ? err.message : "超时或服务不可用");
            dbus_error_free(&err);
            return false;
        }
        dbus_message_unref(reply);
        return true;
    }

    // ---- 绑定流程 ----

    bool createSession(std::string* sessionOut, std::string* error) {
        const std::string token = nextToken("create");
        const std::string requestPath = makeRequestPath(token);

        DBusMessage* msg = dbus_message_new_method_call(
            kPortalBusName, kPortalObjectPath, kGlobalShortcutsIface,
            "CreateSession");
        if (msg == nullptr) {
            *error = "创建 CreateSession 消息失败";
            return false;
        }
        DBusMessageIter it;
        DBusMessageIter dict;
        dbus_message_iter_init_append(msg, &it);
        dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
        appendStringVariant(&dict, "handle_token", token);
        appendStringVariant(&dict, "session_handle_token", token);
        dbus_message_iter_close_container(&it, &dict);

        // 先装好等待状态，再发请求（避免快速响应被丢失的竞争）。
        prepareWait(requestPath);

        std::string callError;
        if (!callPortal("CreateSession", msg, &callError)) {
            std::lock_guard<std::mutex> lock(responseMutex);
            expectedPath.clear();
            *error = callError;
            return false;
        }

        dbus_uint32_t code = 0;
        std::map<std::string, std::string> results;
        if (!awaitResponse(&code, &results, kResponseTimeoutMs)) {
            *error = "CreateSession 超时：门户无响应（xdg-desktop-portal 未运行？）";
            return false;
        }
        if (code != 0) {
            *error = "CreateSession 被拒绝（response=" + std::to_string(code) + "）";
            return false;
        }
        const auto handle = results.find("session_handle");
        if (handle == results.end()) {
            *error = "CreateSession 响应缺少 session_handle";
            return false;
        }
        // 规范注记：session_handle "is an object path that was erroneously
        // implemented as s"（为向后兼容保持字符串类型）。
        *sessionOut = handle->second;
        return true;
    }

    bool bindShortcut(const std::string& sessionHandle, const HotkeySpec& spec,
                      std::string* error) {
        const std::string token = nextToken("bind");
        const std::string requestPath = makeRequestPath(token);

        DBusMessage* msg = dbus_message_new_method_call(
            kPortalBusName, kPortalObjectPath, kGlobalShortcutsIface,
            "BindShortcuts");
        if (msg == nullptr) {
            *error = "创建 BindShortcuts 消息失败";
            return false;
        }

        DBusMessageIter it;
        DBusMessageIter shortcuts;
        DBusMessageIter entry;
        DBusMessageIter props;
        DBusMessageIter options;

        // 按规范签名逐字序列化（已对照官方文档核实）：
        //   BindShortcuts(IN o session_handle, IN a(sa{sv}) shortcuts,
        //                 IN s parent_window, IN a{sv} options) → o
        // 参数顺序必须与签名一致；早期版本把 shortcuts 的签名误写成
        // a(sa(sv))（struct 数组而非字典），libdbus 在序列化时直接断言
        // 崩溃："Writing an element of type dict_entry, but the expected
        // type here is struct"。
        dbus_message_iter_init_append(msg, &it);
        // ① o session_handle
        const char* sessionRaw = sessionHandle.c_str();
        dbus_message_iter_append_basic(&it, DBUS_TYPE_OBJECT_PATH, &sessionRaw);
        // ② a(sa{sv}) shortcuts
        dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "(sa{sv})",
                                         &shortcuts);
        dbus_message_iter_open_container(&shortcuts, DBUS_TYPE_STRUCT, nullptr,
                                         &entry);
        const char* id2 = spec.id.c_str();
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &id2);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_ARRAY, "{sv}",
                                         &props);
        appendStringVariant(&props, "description",
                            "J2 全局热键（" + describeHotkey(spec) + "）");
        appendStringVariant(&props, "preferred_trigger",
                            portalTriggerString(spec));
        dbus_message_iter_close_container(&entry, &props);
        dbus_message_iter_close_container(&shortcuts, &entry);
        dbus_message_iter_close_container(&it, &shortcuts);
        // ③ s parent_window：空串表示无父窗口（托盘应用）。
        const char* parent = "";
        dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &parent);
        // ④ a{sv} options：handle_token。
        dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &options);
        appendStringVariant(&options, "handle_token", token);
        dbus_message_iter_close_container(&it, &options);

        // 先装好等待状态，再发请求（避免快速响应被丢失的竞争）。
        prepareWait(requestPath);

        std::string callError;
        if (!callPortal("BindShortcuts", msg, &callError)) {
            std::lock_guard<std::mutex> lock(responseMutex);
            expectedPath.clear();
            *error = callError;
            return false;
        }

        dbus_uint32_t code = 0;
        std::map<std::string, std::string> results;
        if (!awaitResponse(&code, &results, kResponseTimeoutMs)) {
            *error = "BindShortcuts 超时：请在桌面环境的快捷键设置中确认"
                     "（GNOME/KDE 可能弹出确认框）";
            return false;
        }
        if (code != 0) {
            *error = "BindShortcuts 被拒绝（response=" + std::to_string(code) +
                     "）：快捷键可能已被占用";
            return false;
        }
        return true;
    }

    // ---- 以下两个 *Blocking 方法只在 worker 线程执行（单线程原则）----

    /// 关闭一个门户会话（绑定失败清理与释放路径共用）。
    void closeSession(const std::string& sessionHandle) {
        if (conn == nullptr || sessionHandle.empty()) {
            return;
        }
        DBusMessage* msg = dbus_message_new_method_call(
            kPortalBusName, sessionHandle.c_str(), kSessionIface, "Close");
        if (msg != nullptr) {
            DBusError err;
            dbus_error_init(&err);
            DBusMessage* reply = dbus_connection_send_with_reply_and_block(
                conn, msg, kCallTimeoutMs, &err);
            dbus_error_free(&err);  // 关闭失败不致命（会话会随进程退出而消失）
            if (reply != nullptr) {
                dbus_message_unref(reply);
            }
            dbus_message_unref(msg);
        }
    }

    bool doGrabBlocking(const HotkeySpec& spec, HotkeyCallback callback,
                        void* userData, std::string* error) {
        // 每个快捷键单独一个会话 —— 官方文档明确 "An application can only
        // attempt to bind shortcuts of a session once"，复用同一会话做第二次
        // 绑定会被门户拒绝。
        std::string sessionHandle;
        if (!createSession(&sessionHandle, error)) {
            return false;
        }
        if (!bindShortcut(sessionHandle, spec, error)) {
            closeSession(sessionHandle);  // 绑定失败：关掉刚建的会话，不泄漏
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(boundMutex);
            bound.push_back(Bound{sessionHandle, spec.id, callback, userData});
            if (std::getenv("JIE2_PORTAL_DEBUG") != nullptr) {
                std::fprintf(stderr,
                             "[portal-debug +%lldms] bound.push_back id=%s (size=%zu)\n",
                             debugMs(), spec.id.c_str(), bound.size());
            }
        }
        return true;
    }

    void doReleaseAllBlocking() {
        std::vector<Bound> closing;
        {
            std::lock_guard<std::mutex> lock(boundMutex);
            closing.swap(bound);
        }
        for (const Bound& entry : closing) {
            closeSession(entry.sessionHandle);
        }
    }
};

// ---------------------------------------------------------------------------
// 公开接口
// ---------------------------------------------------------------------------

WaylandPortalHotkeys::WaylandPortalHotkeys() : impl_(new Impl()) {}

WaylandPortalHotkeys::~WaylandPortalHotkeys() = default;

bool WaylandPortalHotkeys::serviceAvailable(std::string* detail) {
    const auto fail = [detail](const std::string& reason) {
        if (detail != nullptr) {
            *detail = reason;
        }
        return false;
    };

    const char* busAddress = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    if (busAddress == nullptr || *busAddress == '\0') {
        // 无会话总线时也尝试系统默认（dbus_bus_get 会给出更具体的原因）——
        // 但门 Pro 服务的可用性判断仍以实际连接为准。
        if (detail != nullptr) {
            *detail = "未设置 DBUS_SESSION_BUS_ADDRESS（无会话总线）";
        }
        return false;
    }

    dbus_threads_init_default();
    DBusError err;
    dbus_error_init(&err);
    DBusConnection* conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
    if (conn == nullptr) {
        const std::string reason =
            std::string("无法连接会话总线：") +
            (dbus_error_is_set(&err) ? err.message : "未知原因");
        dbus_error_free(&err);
        return fail(reason);
    }
    dbus_connection_set_exit_on_disconnect(conn, FALSE);

    // ① 服务名是否有属主。
    bool hasOwner = false;
    {
        DBusMessage* msg = dbus_message_new_method_call(
            "org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus", "NameHasOwner");
        if (msg != nullptr) {
            const char* name = kPortalBusName;
            dbus_message_append_args(msg, DBUS_TYPE_STRING, &name,
                                     DBUS_TYPE_INVALID);
            DBusMessage* reply = dbus_connection_send_with_reply_and_block(
                conn, msg, 2000, &err);
            dbus_message_unref(msg);
            if (reply != nullptr) {
                dbus_bool_t owned = FALSE;
                if (dbus_message_get_args(reply, &err, DBUS_TYPE_BOOLEAN, &owned,
                                          DBUS_TYPE_INVALID)) {
                    hasOwner = owned != FALSE;
                }
                dbus_message_unref(reply);
            }
            if (dbus_error_is_set(&err)) {
                dbus_error_free(&err);
            }
        }
    }
    if (!hasOwner) {
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        return fail("org.freedesktop.portal.Desktop 未在会话总线上运行"
                    "（未安装或未启动 xdg-desktop-portal）");
    }

    // ② 读取 GlobalShortcuts 接口版本（证实接口存在，而不只是服务存在）。
    std::string versionText = "未知";
    {
        DBusMessage* msg = dbus_message_new_method_call(
            kPortalBusName, kPortalObjectPath,
            "org.freedesktop.DBus.Properties", "Get");
        if (msg != nullptr) {
            const char* iface = kGlobalShortcutsIface;
            const char* prop = "version";
            dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface,
                                     DBUS_TYPE_STRING, &prop,
                                     DBUS_TYPE_INVALID);
            DBusMessage* reply = dbus_connection_send_with_reply_and_block(
                conn, msg, 2000, &err);
            dbus_message_unref(msg);
            if (reply != nullptr) {
                DBusMessageIter it;
                if (dbus_message_iter_init(reply, &it) &&
                    dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_VARIANT) {
                    DBusMessageIter var;
                    dbus_message_iter_recurse(&it, &var);
                    if (dbus_message_iter_get_arg_type(&var) ==
                        DBUS_TYPE_UINT32) {
                        dbus_uint32_t v = 0;
                        dbus_message_iter_get_basic(&var, &v);
                        versionText = std::to_string(v);
                    }
                }
                dbus_message_unref(reply);
            }
            if (dbus_error_is_set(&err)) {
                dbus_error_free(&err);
            }
        }
    }

    dbus_connection_close(conn);
    dbus_connection_unref(conn);
    if (detail != nullptr) {
        *detail = "GlobalShortcuts 接口可用（version=" + versionText + "）";
    }
    return true;
}

bool WaylandPortalHotkeys::grab(const HotkeySpec& spec,
                                HotkeyCallback callback, void* userData,
                                std::string* error) {
    // 启动（连接 + 信号匹配 + 工作线程）在调用线程完成。
    // 此刻 worker 尚未启动，调用线程对连接是独占的，无并发问题。
    std::string startError;
    if (!impl_->ensureStarted(&startError)) {
        if (error != nullptr) {
            *error = startError;
        }
        return false;
    }

    // 入队 + 等 worker 完成（worker 是唯一的 D-Bus dispatch 线程）。
    auto request = std::make_shared<Impl::Request>();
    request->kind = Impl::Request::Kind::Grab;
    request->spec = spec;
    request->callback = callback;
    request->userData = userData;
    {
        std::lock_guard<std::mutex> lock(impl_->queueMutex);
        impl_->queue.push_back(request);
    }
    impl_->queueCv.notify_all();

    std::unique_lock<std::mutex> lock(impl_->queueMutex);
    // 内部 awaitResponse 有 12 秒上限；这里给更宽余量让内部超时先给出精确错误。
    const bool completed = impl_->doneCv.wait_for(
        lock, std::chrono::seconds(25), [&request] { return request->done; });
    if (!completed) {
        if (error != nullptr) {
            *error = "门户绑定超时（等待工作线程完成超时）";
        }
        return false;
    }
    if (!request->ok && error != nullptr) {
        *error = request->error;
    }
    return request->ok;
}

void WaylandPortalHotkeys::releaseAll() {
    if (!impl_->running.load()) {
        return;  // 从未启用过：没有会话需要关闭
    }
    auto request = std::make_shared<Impl::Request>();
    request->kind = Impl::Request::Kind::ReleaseAll;
    {
        std::lock_guard<std::mutex> lock(impl_->queueMutex);
        impl_->queue.push_back(request);
    }
    impl_->queueCv.notify_all();

    std::unique_lock<std::mutex> lock(impl_->queueMutex);
    impl_->doneCv.wait_for(lock, std::chrono::seconds(15),
                           [&request] { return request->done; });
}

} // namespace jie2::platform
