#include "platform/linux/wayland_screenshot.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

#include <dbus/dbus.h>

#include "platform/linux/png_decode.h"

namespace jie2::platform {

namespace {

constexpr const char* kPortalBusName = "org.freedesktop.portal.Desktop";
constexpr const char* kPortalObjectPath = "/org/freedesktop/portal/desktop";
constexpr const char* kScreenshotIface = "org.freedesktop.portal.Screenshot";
constexpr const char* kRequestIface = "org.freedesktop.portal.Request";

int kCallTimeoutMs = 5000;

/// 一次截图请求的捕获状态（filter 写入，轮询读取 —— 同一线程内）。
struct CaptureContext {
    std::string expectedPath;
    bool responseReady = false;
    dbus_uint32_t responseCode = 0;
    std::string uri;
};

/// 解析响应 a{sv} 里我们关心的字符串字段（目前只有 uri）。
void collectStringVariants(DBusMessageIter* arrayIter,
                           CaptureContext* ctx) {
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
            if (key != nullptr && type == DBUS_TYPE_STRING) {
                const char* value = nullptr;
                dbus_message_iter_get_basic(&var, &value);
                if (value != nullptr && std::strcmp(key, "uri") == 0) {
                    ctx->uri = value;
                }
            }
        }
        dbus_message_iter_next(&array);
    }
}

DBusHandlerResult filterCallback(DBusConnection*, DBusMessage* msg, void* data) {
    auto* ctx = static_cast<CaptureContext*>(data);
    if (ctx == nullptr ||
        !dbus_message_is_signal(msg, kRequestIface, "Response")) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    const char* path = dbus_message_get_path(msg);
    if (path == nullptr || ctx->expectedPath != path) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    DBusMessageIter it;
    if (!dbus_message_iter_init(msg, &it)) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    dbus_uint32_t code = 0;
    if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_UINT32) {
        dbus_message_iter_get_basic(&it, &code);
    }
    dbus_message_iter_next(&it);
    if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
        collectStringVariants(&it, ctx);
    }
    ctx->responseCode = code;
    ctx->responseReady = true;
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/// 打开 {sv} 字典条目：key → 字符串 / 布尔 variant。
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

void appendBoolVariant(DBusMessageIter* dict, const char* key, bool value) {
    DBusMessageIter entry;
    DBusMessageIter var;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &var);
    dbus_bool_t raw = value ? TRUE : FALSE;
    dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &raw);
    dbus_message_iter_close_container(&entry, &var);
    dbus_message_iter_close_container(dict, &entry);
}

/// "file:///run/user/1000/doc/x/y.png" → "/run/user/1000/doc/x/y.png"。
/// 处理空 host（file:///...）与 "localhost"（file://localhost/...）。
bool fileUriToPath(const std::string& uri, std::string* path,
                   std::string* error) {
    const std::string prefix = "file://";
    if (uri.rfind(prefix, 0) != 0) {
        *error = "截图门户返回的 URI 不是 file:// 形式：" + uri;
        return false;
    }
    std::string rest = uri.substr(prefix.size());
    if (rest.rfind("localhost/", 0) == 0) {
        rest = rest.substr(std::strlen("localhost"));
    } else if (!rest.empty() && rest[0] != '/') {
        // 非空 host（如 file://host/path）—— 门户不会这样返回，如实报错。
        *error = "截图门户返回的 URI 含非本地主机：" + uri;
        return false;
    }
    if (rest.empty() || rest[0] != '/') {
        *error = "截图门户返回的 URI 缺少路径：" + uri;
        return false;
    }
    *path = rest;
    return true;
}

} // namespace

bool WaylandScreenshotPortal::serviceAvailable(std::string* detail) {
    const auto fail = [detail](const std::string& reason) {
        if (detail != nullptr) {
            *detail = reason;
        }
        return false;
    };

    const char* busAddress = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    if (busAddress == nullptr || *busAddress == '\0') {
        return fail("未设置 DBUS_SESSION_BUS_ADDRESS（无会话总线）");
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
        return fail("org.freedesktop.portal.Desktop 未在会话总线上运行");
    }

    std::string versionText = "未知";
    {
        DBusMessage* msg = dbus_message_new_method_call(
            kPortalBusName, kPortalObjectPath,
            "org.freedesktop.DBus.Properties", "Get");
        if (msg != nullptr) {
            const char* iface = kScreenshotIface;
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
        *detail = "Screenshot 接口可用（version=" + versionText + "）";
    }
    return true;
}

bool WaylandScreenshotPortal::captureFullscreen(core::ImageBuffer* out,
                                                std::string* error,
                                                int timeoutMs) const {
    const auto fail = [error](const std::string& reason) {
        if (error != nullptr) {
            *error = reason;
        }
        return false;
    };

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

    // 预测 request 路径（规范保证：handle_token 就是为了让客户端预测路径）。
    const std::string token = "jie2_shot_1";
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
    const std::string requestPath =
        "/org/freedesktop/portal/desktop/request/" + senderToken + "/" + token;

    CaptureContext ctx;
    ctx.expectedPath = requestPath;
    dbus_connection_add_filter(conn, &filterCallback, &ctx, nullptr);
    dbus_bus_add_match(conn,
                       "type='signal',interface='org.freedesktop.portal.Request',"
                       "member='Response'",
                       &err);
    if (dbus_error_is_set(&err)) {
        dbus_error_free(&err);
    }
    dbus_connection_flush(conn);

    // 调用 Screenshot(parent_window="", options{handle_token, interactive=false})。
    DBusMessage* msg = dbus_message_new_method_call(
        kPortalBusName, kPortalObjectPath, kScreenshotIface, "Screenshot");
    if (msg == nullptr) {
        dbus_connection_remove_filter(conn, &filterCallback, &ctx);
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        return fail("创建 Screenshot 消息失败");
    }
    {
        DBusMessageIter it;
        DBusMessageIter options;
        dbus_message_iter_init_append(msg, &it);
        const char* parent = "";
        dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &parent);
        dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &options);
        appendStringVariant(&options, "handle_token", token);
        appendBoolVariant(&options, "interactive", false);
        dbus_message_iter_close_container(&it, &options);
    }

    DBusMessage* reply = dbus_connection_send_with_reply_and_block(
        conn, msg, kCallTimeoutMs, &err);
    dbus_message_unref(msg);
    if (reply == nullptr) {
        const std::string reason =
            std::string("Screenshot 调用失败：") +
            (dbus_error_is_set(&err) ? err.message : "超时或服务不可用");
        dbus_error_free(&err);
        dbus_connection_remove_filter(conn, &filterCallback, &ctx);
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        return fail(reason);
    }
    dbus_message_unref(reply);

    // 等 Response（可能包含用户在 GNOME 授权框上的确认耗时）。
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline && !ctx.responseReady) {
        dbus_connection_read_write_dispatch(conn, 50);
    }

    const bool ready = ctx.responseReady;
    const dbus_uint32_t code = ctx.responseCode;
    const std::string uri = ctx.uri;
    dbus_connection_remove_filter(conn, &filterCallback, &ctx);
    dbus_connection_close(conn);
    dbus_connection_unref(conn);

    if (!ready) {
        return fail("截图门户超时（" + std::to_string(timeoutMs) +
                    "ms 内未响应）：可能在等待用户确认授权");
    }
    if (code != 0) {
        return fail("截图门户返回失败（response=" + std::to_string(code) +
                    "）：用户拒绝或门户错误");
    }
    if (uri.empty()) {
        return fail("截图门户响应缺少 uri 字段");
    }

    // 读 PNG 文件并解码。
    std::string filePath;
    std::string uriError;
    if (!fileUriToPath(uri, &filePath, &uriError)) {
        return fail(uriError);
    }
    std::ifstream input(filePath, std::ios::binary);
    if (!input) {
        return fail("无法打开截图文件：" + filePath);
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                                    std::istreambuf_iterator<char>());
    input.close();
    if (bytes.empty()) {
        return fail("截图文件为空：" + filePath);
    }

    std::string decodeError;
    if (!decodePng(bytes.data(), bytes.size(), out, &decodeError)) {
        return fail("截图 PNG 解码失败：" + decodeError);
    }
    return true;
}

} // namespace jie2::platform
