#!/usr/bin/env python3
"""mock xdg-desktop-portal · GlobalShortcuts 门户（仅用于验证，不进产品）。

模拟真实门户的协议行为，供容器内端到端验证（真实门户需要完整桌面会话）：

  CreateSession(options) -> request path
      随后在 request 对象上发 Response(0, {session_handle: <path>})
  BindShortcuts(session, shortcuts, parent, options) -> request path
      随后 Response(0, {})；延迟 MOCK_TRIGGER_DELAY_MS 后对第一个绑定
      发 Activated 信号（模拟用户按下热键）。
  Session.Close()  关闭会话。
  Properties.Get("org.freedesktop.portal.GlobalShortcuts", "version")

环境变量：
  MOCK_TRIGGER_DELAY_MS  绑定后多久发 Activated（默认 1500；0 = 不发）
  MOCK_RESPONSE_CODE     Response 的 code（默认 0=成功；可设 1/2 模拟拒绝）

用法（在 dbus-run-session 内）：
  python3 mock-portal.py &
"""
import os
import sys

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

PORTAL_BUS = "org.freedesktop.portal.Desktop"
PORTAL_PATH = "/org/freedesktop/portal/desktop"
GS_IFACE = "org.freedesktop.portal.GlobalShortcuts"
SCREENSHOT_IFACE = "org.freedesktop.portal.Screenshot"
REQ_IFACE = "org.freedesktop.portal.Request"
SESSION_IFACE = "org.freedesktop.portal.Session"

TRIGGER_DELAY_MS = int(os.environ.get("MOCK_TRIGGER_DELAY_MS", "1500"))
RESPONSE_CODE = int(os.environ.get("MOCK_RESPONSE_CODE", "0"))

# 截图接口：返回哪个 file:// URI（由测试脚本准备一张真实 PNG），
# 以及模拟"用户确认授权"的延迟。
SCREENSHOT_URI = os.environ.get("MOCK_SCREENSHOT_URI", "")
SCREENSHOT_DELAY_MS = int(os.environ.get("MOCK_SCREENSHOT_DELAY_MS", "200"))


def sender_token(sender):
    """唯一名 → 门户路径里的 token（":1.42" → "1_42"）。"""
    return str(sender or ":0.0").lstrip(":").replace(".", "_")


class RequestObject(dbus.service.Object):
    def __init__(self, bus, path):
        super().__init__(bus, path)

    def respond_later(self, code, results, delay_ms=50):
        def fire():
            self.Response(dbus.UInt32(code), results)
            return False
        GLib.timeout_add(delay_ms, fire)

    @dbus.service.signal(REQ_IFACE, signature="ua{sv}")
    def Response(self, response, results):
        pass


class SessionObject(dbus.service.Object):
    def __init__(self, bus, path, portal):
        super().__init__(bus, path)
        self._portal = portal

    @dbus.service.method(SESSION_IFACE, in_signature="", out_signature="")
    def Close(self):
        self._portal.session_closed(self._object_path)

    def emit_activated(self, shortcut_id):
        self.Activated(self._object_path, shortcut_id, dbus.UInt64(0), {})

    @dbus.service.signal(GS_IFACE, signature="osta{sv}")
    def Activated(self, session_handle, shortcut_id, timestamp, options):
        pass


class PortalObject(dbus.service.Object):
    def __init__(self, bus):
        super().__init__(bus, PORTAL_PATH)
        self._bus = bus
        self._counter = 0
        self._sessions = {}
        self._shortcut_ids = []

    # ---- 辅助 ----
    #
    # 调用者唯一名通过 dbus-python 的 sender_keyword 传入 ——
    # 不要用 dbus.get_current_message()（该 API 不存在，实测报
    # AttributeError；sender_keyword 是 dbus-python 的正式方式）。

    def _make_request(self, options, sender):
        self._counter += 1
        token = str(options.get("handle_token", "req%d" % self._counter))
        path = "/org/freedesktop/portal/desktop/request/%s/%s" % (
            sender_token(sender), token)
        return RequestObject(self._bus, path)

    # ---- GlobalShortcuts 接口 ----

    @dbus.service.method(GS_IFACE, in_signature="a{sv}", out_signature="o",
                         sender_keyword="sender")
    def CreateSession(self, options, sender=None):
        session_token = str(
            options.get("session_handle_token", "sess%d" % self._counter))
        session_path = "/org/freedesktop/portal/desktop/session/%s/%s" % (
            sender_token(sender), session_token)

        req = self._make_request(options, sender)
        session = SessionObject(self._bus, session_path, self)
        self._sessions[session_path] = session
        # 规范注记：session_handle "is an object path that was erroneously
        # implemented as s" —— 为向后兼容保持**字符串**类型（不能发 ObjectPath）。
        req.respond_later(RESPONSE_CODE, {
            "session_handle": dbus.String(session_path),
        })
        print("[mock-portal] CreateSession -> %s" % session_path, flush=True)
        return dbus.ObjectPath(req._object_path)

    # 签名必须与规范逐字一致：a(sa{sv})（字典），不是 a(sa(sv))（结构体数组）。
    @dbus.service.method(GS_IFACE, in_signature="oa(sa{sv})sa{sv}",
                         out_signature="o", sender_keyword="sender")
    def BindShortcuts(self, session_handle, shortcuts, parent_window, options,
                      sender=None):
        session = self._sessions.get(str(session_handle))
        req = self._make_request(options, sender)
        ids = []
        for sc in shortcuts:
            sid = str(sc[0])
            ids.append(sid)
            props = dict(sc[1]) if len(sc) > 1 else {}
            trigger = str(props.get("preferred_trigger", "?"))
            print("[mock-portal] BindShortcuts id=%s trigger=%s" % (sid, trigger),
                  flush=True)
        self._shortcut_ids = ids
        req.respond_later(RESPONSE_CODE, {})

        # 模拟"用户按下热键"：延迟发一次 Activated。
        if TRIGGER_DELAY_MS > 0 and session is not None and ids:
            def fire():
                session.emit_activated(ids[0])
                print("[mock-portal] Activated %s" % ids[0], flush=True)
                return False
            GLib.timeout_add(TRIGGER_DELAY_MS, fire)
        return dbus.ObjectPath(req._object_path)

    # ---- Screenshot 接口 ----

    @dbus.service.method(SCREENSHOT_IFACE, in_signature="sa{sv}",
                         out_signature="o", sender_keyword="sender")
    def Screenshot(self, parent_window, options, sender=None):
        req = self._make_request(options, sender)
        interactive = bool(options.get("interactive", False))
        print("[mock-portal] Screenshot interactive=%s -> %s" % (
            interactive, SCREENSHOT_URI or "(none)"), flush=True)
        if SCREENSHOT_URI:
            req.respond_later(RESPONSE_CODE,
                              {"uri": dbus.String(SCREENSHOT_URI)},
                              delay_ms=SCREENSHOT_DELAY_MS)
        else:
            # 没配置测试图：模拟"用户取消"（response=1）。
            req.respond_later(1, {}, delay_ms=SCREENSHOT_DELAY_MS)
        return dbus.ObjectPath(req._object_path)

    # ---- Properties（能力探测用）----

    @dbus.service.method("org.freedesktop.DBus.Properties",
                         in_signature="ss", out_signature="v")
    def Get(self, interface, prop):
        if interface == GS_IFACE and prop == "version":
            return dbus.UInt32(2)  # 规范当前版本（ConfigureShortcuts 为 v2 新增）
        if interface == SCREENSHOT_IFACE and prop == "version":
            return dbus.UInt32(2)  # Screenshot 规范当前版本
        raise dbus.exceptions.DBusException(
            "org.freedesktop.DBus.Error.InvalidArgs", "Unknown property")

    # ---- 会话生命周期 ----

    def session_closed(self, path):
        self._sessions.pop(path, None)
        print("[mock-portal] session closed: %s" % path, flush=True)


def main():
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SessionBus()
    # 关键：BusName 必须保持引用，否则对象被 GC → 名字释放 →
    # 客户端报 "The name ... was not provided by any .service files"
    # （实测踩过：构造函数返回值没接住，mock 看着"启动了"却没人持有名字）。
    bus_name = dbus.service.BusName(PORTAL_BUS, bus)
    portal = PortalObject(bus)
    print("[mock-portal] ready (trigger=%dms response_code=%d shot_uri=%s)" % (
        TRIGGER_DELAY_MS, RESPONSE_CODE, SCREENSHOT_URI or "(none)"), flush=True)
    # 引用保活：模块级对象在 MainLoop 运行期间持续存在。
    _keepalive = (bus_name, portal)
    GLib.MainLoop().run()
    del _keepalive
    return 0


if __name__ == "__main__":
    sys.exit(main())
