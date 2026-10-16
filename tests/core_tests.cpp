// 「截2」图像内核单元测试
// 不依赖 Qt 与平台 API，可在任何环境（含无桌面会话的 CI 容器）运行。

#include <cstdio>
#include <string>

#include "core/annotation.h"
#include "core/capability.h"
#include "core/capture_history.h"
#include "core/config.h"
#include "core/geometry.h"
#include "core/image_buffer.h"
#include "core/image_ops.h"
#include "core/snap.h"

namespace {

int g_failures = 0;

void check(bool condition, const char* expression, const char* file, int line) {
    if (!condition) {
        std::printf("FAIL %s:%d: %s\n", file, line, expression);
        ++g_failures;
    }
}

#define CHECK(cond) check((cond), #cond, __FILE__, __LINE__)

void testGeometry() {
    using namespace jie2::core;

    const Rect a{0, 0, 100, 100};
    const Rect b{50, 50, 100, 100};
    const Rect overlap = a.intersected(b);
    CHECK(overlap.x == 50);
    CHECK(overlap.y == 50);
    CHECK(overlap.width == 50);
    CHECK(overlap.height == 50);

    CHECK(a.contains({0, 0}));
    CHECK(a.contains({99, 99}));
    CHECK(!a.contains({100, 100}));
    CHECK(!a.contains({-1, 0}));

    // 注意：花括号初始化列表出现在宏实参里时需要额外括号。
    CHECK((Rect{0, 0, 0, 10}.isEmpty()));
    CHECK((!Rect{0, 0, 1, 1}.isEmpty()));

    const Rect noOverlap = a.intersected(Rect{200, 200, 10, 10});
    CHECK(noOverlap.isEmpty());
}

void testImageBuffer() {
    using namespace jie2::core;

    ImageBuffer image = ImageBuffer::filled(4, 3, Rgba{10, 20, 30, 40});
    CHECK(image.width() == 4);
    CHECK(image.height() == 3);
    CHECK(image.byteSize() == 4u * 3u * 4u);
    CHECK(!image.isEmpty());

    const Rgba px = image.pixel(1, 1);
    CHECK(px.r == 10 && px.g == 20 && px.b == 30 && px.a == 40);

    // 越界返回透明黑，而不是崩溃或读到邻近内存
    const Rgba oob = image.pixel(-1, 0);
    CHECK(oob.r == 0 && oob.g == 0 && oob.b == 0 && oob.a == 0);

    // BGRA 通道解释顺序
    ImageBuffer bgra(1, 1, PixelFormat::BGRA8888);
    bgra.data()[0] = 3;    // 内存首字节是 B
    bgra.data()[1] = 2;
    bgra.data()[2] = 1;    // R
    bgra.data()[3] = 255;
    const Rgba converted = bgra.pixel(0, 0);
    CHECK(converted.r == 1 && converted.g == 2 && converted.b == 3 &&
          converted.a == 255);

    ImageBuffer empty;
    CHECK(empty.isEmpty());
    CHECK(empty.byteSize() == 0);
}

void testConfig() {
    using namespace jie2::core;

    Config config;
    config.set("hotkey.count", "3");
    config.set("platform", "linux");
    config.set("flag", "true");

    const std::string text = config.serialize();
    const Config parsed = Config::parse(text);
    CHECK(parsed.getInt("hotkey.count", 0) == 3);
    CHECK(parsed.get("platform", "") == "linux");
    CHECK(parsed.getBool("flag", false));
    CHECK(parsed.getInt("missing", 42) == 42);
    CHECK(parsed.get("missing", "fallback") == "fallback");

    // 宽松解析：注释与空行忽略、坏行跳过、首尾空白裁剪、空键跳过
    const Config messy = Config::parse(
        "# comment\n"
        "  good = 1  \n"
        "bogus line without equals\n"
        "=2\n"
        "\n"
        "x=3\n");
    CHECK(messy.getInt("good", 0) == 1);
    CHECK(messy.getInt("x", 0) == 3);
    CHECK(messy.entries().size() == 2);

    // 非法值回退而非抛异常
    Config bad;
    bad.set("n", "abc");
    CHECK(bad.getInt("n", 7) == 7);
}

void testCapability() {
    using namespace jie2::core;

    PlatformCapabilities limited;
    limited.session = SessionType::Wayland;
    limited.sessionDetail = "KDE (Wayland)";
    limited.screenCapture = {CapabilityStatus::Degraded, "portal 需授权"};
    limited.globalHotkey = {CapabilityStatus::Unavailable, "未实现"};
    limited.alwaysOnTop = {CapabilityStatus::Degraded, ""};
    limited.layerShell = {CapabilityStatus::Degraded, ""};
    limited.clipboardImage = {CapabilityStatus::Available, ""};

    CHECK(limited.hasLimitations());
    const std::string text = limited.describe();
    CHECK(text.find("Wayland") != std::string::npos);
    CHECK(text.find("portal") != std::string::npos);

    PlatformCapabilities healthy;
    healthy.session = SessionType::MacOS;
    healthy.screenCapture = {CapabilityStatus::Available, ""};
    healthy.globalHotkey = {CapabilityStatus::Available, ""};
    healthy.alwaysOnTop = {CapabilityStatus::Available, ""};
    healthy.layerShell = {CapabilityStatus::NotApplicable, ""};
    healthy.clipboardImage = {CapabilityStatus::Available, ""};
    CHECK(!healthy.hasLimitations());
}

void testImageOps() {
    using namespace jie2::core;

    // crop：基本裁剪与越界裁剪
    ImageBuffer source = ImageBuffer::filled(10, 8, Rgba{1, 2, 3, 255});
    ImageBuffer patch = crop(source, Rect{2, 3, 4, 2});
    CHECK(patch.width() == 4);
    CHECK(patch.height() == 2);
    const Rgba px = patch.pixel(0, 0);
    CHECK(px.r == 1 && px.g == 2 && px.b == 3 && px.a == 255);

    ImageBuffer clipped = crop(source, Rect{8, 6, 10, 10});
    CHECK(clipped.width() == 2);
    CHECK(clipped.height() == 2);

    CHECK(crop(source, Rect{100, 100, 5, 5}).isEmpty());

    // crop 不改动源图
    CHECK(source.pixel(2, 3).r == 1);

    // blit：贴图与越界保护
    ImageBuffer canvas = ImageBuffer::filled(6, 6, Rgba{0, 0, 0, 255});
    ImageBuffer stamp = ImageBuffer::filled(3, 3, Rgba{9, 8, 7, 255});
    blit(canvas, stamp, Point{1, 1});
    CHECK(canvas.pixel(1, 1).r == 9);
    CHECK(canvas.pixel(3, 3).r == 9);
    CHECK(canvas.pixel(4, 4).r == 0);
    CHECK(canvas.pixel(0, 0).r == 0);

    // blit 负坐标：只贴可视部分
    ImageBuffer canvas2 = ImageBuffer::filled(4, 4, Rgba{0, 0, 0, 255});
    blit(canvas2, stamp, Point{-1, -1});
    CHECK(canvas2.pixel(0, 0).r == 9);
    CHECK(canvas2.pixel(1, 1).r == 9);
    CHECK(canvas2.pixel(2, 2).r == 0);

    // scaleNearest：尺寸正确、颜色保持
    ImageBuffer scaled = scaleNearest(stamp, 6, 6);
    CHECK(scaled.width() == 6);
    CHECK(scaled.height() == 6);
    CHECK(scaled.pixel(0, 0).r == 9);
    CHECK(scaled.pixel(5, 5).r == 9);

    ImageBuffer down = scaleNearest(stamp, 1, 1);
    CHECK(down.width() == 1);
    CHECK(down.pixel(0, 0).r == 9);

    // comparePixels：相同与不同的判定
    const PixelDiff same = comparePixels(stamp, stamp);
    CHECK(same.maxChannelDelta == 0);
    CHECK(same.differingPixels == 0);

    ImageBuffer changed = stamp;
    changed.data()[0] = 100;  // 改动第一个通道
    const PixelDiff diff = comparePixels(stamp, changed);
    CHECK(diff.maxChannelDelta == 91);  // |9 - 100|
    CHECK(diff.differingPixels == 1);

    // 尺寸不同视为完全不同
    const PixelDiff mismatch = comparePixels(stamp, scaled);
    CHECK(mismatch.maxChannelDelta == 255);
}

void testImageOpsDecorations() {
    using namespace jie2::core;

    const Rgba red{255, 0, 0, 255};
    const Rgba blue{0, 0, 255, 255};
    const Rgba white{255, 255, 255, 255};

    // --- addBorder：2x2 红图 + 1px 蓝框 = 4x4 ---
    {
        const ImageBuffer src = ImageBuffer::filled(2, 2, red);
        const ImageBuffer out = addBorder(src, 1, blue);
        CHECK(out.width() == 4 && out.height() == 4);
        CHECK(out.pixel(0, 0) == blue);
        CHECK(out.pixel(3, 3) == blue);
        CHECK(out.pixel(1, 1) == red);
        CHECK(out.pixel(2, 2) == red);

        // width=0 时不处理
        const ImageBuffer none = addBorder(src, 0, blue);
        CHECK(none.width() == 2 && none.height() == 2);
    }

    // --- roundCorners：角上 alpha 衰减，中心保持 ---
    {
        const ImageBuffer src = ImageBuffer::filled(16, 16, white);
        const ImageBuffer out = roundCorners(src, 6);
        CHECK(out.width() == 16 && out.height() == 16);
        CHECK(out.pixel(8, 8).a == 255);
        // 角点 (0,0)：距圆心 (5.5,5.5) 距离约 7.78 > 6，应几乎全透明
        CHECK(out.pixel(0, 0).a < 32);
        // 半径=0 时原样
        const ImageBuffer none = roundCorners(src, 0);
        CHECK(none.pixel(0, 0).a == 255);
    }

    // --- pixelate：整块平均 ---
    {
        // 4x4：上两行黑、下两行白
        ImageBuffer src(4, 4);
        for (int y = 0; y < 4; ++y) {
            const Rgba color = y < 2 ? Rgba{0, 0, 0, 255} : white;
            for (int x = 0; x < 4; ++x) {
                std::uint8_t* p = src.scanline(y) + static_cast<std::size_t>(x) * 4;
                p[0] = color.r; p[1] = color.g; p[2] = color.b; p[3] = color.a;
            }
        }
        // blockSize=4：全图一块 → 平均 ≈ 127/128
        const ImageBuffer out = pixelate(src, Rect{0, 0, 4, 4}, 4);
        const int avg = out.pixel(0, 0).r;
        CHECK(avg == 127 || avg == 128);
        CHECK(out.pixel(3, 3).r == avg);  // 全块同色

        // blockSize=2：上下各自成块 → 保持黑白
        const ImageBuffer out2 = pixelate(src, Rect{0, 0, 4, 4}, 2);
        CHECK(out2.pixel(0, 0).r == 0);
        CHECK(out2.pixel(0, 3).r == 255);
    }

    // --- boxBlur：区域内被平滑，区域外不变 ---
    {
        // 8x8：左半黑右半白
        ImageBuffer src(8, 8);
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                const Rgba color = x < 4 ? Rgba{0, 0, 0, 255} : white;
                std::uint8_t* p = src.scanline(y) + static_cast<std::size_t>(x) * 4;
                p[0] = color.r; p[1] = color.g; p[2] = color.b; p[3] = color.a;
            }
        }
        const ImageBuffer out = boxBlur(src, Rect{0, 0, 8, 8}, 3);
        // 边界附近（x=3/4）应出现中间值
        const int edge = out.pixel(3, 4).r;
        CHECK(edge > 0 && edge < 255);
        // 远离边界处仍接近原色
        CHECK(out.pixel(0, 4).r < 80);
        CHECK(out.pixel(7, 4).r > 175);

        // 区域外不变：只模糊右半，则左半原样
        const ImageBuffer partial = boxBlur(src, Rect{4, 0, 4, 8}, 3);
        CHECK(partial.pixel(0, 4).r == 0);
        CHECK(partial.pixel(2, 4).r == 0);
    }

    // --- addShadow：画布扩大，原图内容保持 ---
    {
        const ImageBuffer src = ImageBuffer::filled(4, 4, red);
        ShadowSpec spec;
        spec.blurRadius = 4;
        spec.offsetX = 0;
        spec.offsetY = 0;
        const ImageBuffer out = addShadow(src, spec);
        const int pad = 4 * 2;  // blur*2 + |offset|
        CHECK(out.width() == 4 + 2 * pad);
        CHECK(out.height() == 4 + 2 * pad);
        // 原图内容在 (pad, pad)
        CHECK(out.pixel(pad + 1, pad + 1) == red);
        // 阴影区（原图外）应有半透明黑色
        const Rgba shadowPixel = out.pixel(pad + 1, 2);
        CHECK(shadowPixel.a > 0);
        CHECK(shadowPixel.r == 0 && shadowPixel.g == 0 && shadowPixel.b == 0);
    }
}

void testCaptureHistory() {
    using namespace jie2::core;

    CaptureHistory history(3);
    CHECK(history.empty());

    auto makeEntry = [](const char* path, int w, int h) {
        HistoryEntry e;
        e.path = path;
        e.timestamp = "20261015-120000";
        e.width = w;
        e.height = h;
        return e;
    };

    history.append(makeEntry("/a.png", 10, 10));
    history.append(makeEntry("/b.png", 20, 20));
    CHECK(history.size() == 2);
    CHECK(history.latest()->path == "/b.png");
    CHECK(history.fromLatest(0)->path == "/b.png");
    CHECK(history.fromLatest(1)->path == "/a.png");
    CHECK(history.fromLatest(2) == nullptr);

    // 容量裁剪：加第三条后仍为 3，第四条挤掉最旧的
    history.append(makeEntry("/c.png", 30, 30));
    CHECK(history.size() == 3);
    history.append(makeEntry("/d.png", 40, 40));
    CHECK(history.size() == 3);
    CHECK(history.fromLatest(2)->path == "/b.png");  // /a.png 已被挤掉

    // 重复路径置顶：再次记录 /b.png 应移到末尾且不增加数量
    history.append(makeEntry("/b.png", 21, 21));
    CHECK(history.size() == 3);
    CHECK(history.latest()->path == "/b.png");
    CHECK(history.latest()->width == 21);

    // 序列化往返
    const std::string text = history.serialize();
    const CaptureHistory restored = CaptureHistory::parse(text, 3);
    CHECK(restored.size() == history.size());
    CHECK(restored.latest()->path == history.latest()->path);
    CHECK(restored.fromLatest(1)->path == history.fromLatest(1)->path);

    // 宽松解析：坏行跳过、空行跳过、注释跳过、路径含分隔符时仍可解析
    const CaptureHistory messy = CaptureHistory::parse(
        "# comment\n"
        "bogus-line\n"
        "\n"
        "/x|y.png|20260101-000000|5|6\n");
    CHECK(messy.size() == 1);
    CHECK(messy.latest()->path == "/x|y.png");
    CHECK(messy.latest()->width == 5 && messy.latest()->height == 6);

    // 空路径忽略
    CaptureHistory fresh(4);
    fresh.append(makeEntry("", 1, 1));
    CHECK(fresh.empty());

    // 缩容
    CaptureHistory shrink(5);
    shrink.append(makeEntry("/1", 1, 1));
    shrink.append(makeEntry("/2", 1, 1));
    shrink.append(makeEntry("/3", 1, 1));
    shrink.setCapacity(2);
    CHECK(shrink.size() == 2);
    CHECK(shrink.latest()->path == "/3");

    // remove：移除指定路径（衣架右键"取下"），移除不存在的不报错。
    CaptureHistory removable(8);
    removable.append(makeEntry("/a", 1, 1));
    removable.append(makeEntry("/b", 1, 1));
    removable.append(makeEntry("/c", 1, 1));
    CHECK(removable.remove("/b"));
    CHECK(removable.size() == 2);
    CHECK(removable.entries()[0].path == "/a");
    CHECK(removable.entries()[1].path == "/c");
    CHECK(!removable.remove("/not-there"));
    CHECK(removable.size() == 2);
}

void testSnap() {
    using namespace jie2::core;

    // 候选：一个窗口 (100,100)-(300,250)。
    const std::vector<Rect> windows{Rect{100, 100, 200, 150}};

    // 拖拽矩形左边缘离窗口左边 3px（阈值 8 内）→ 左吸附。
    {
        const SnapResult snapped =
            snapRect(Rect{103, 130, 50, 50}, windows, 8);
        CHECK(snapped.snappedLeft);
        CHECK(snapped.rect.x == 100);
        CHECK(snapped.rect.width == 53);  // 右边不动
    }

    // 右边缘靠近窗口左边 → 右吸附（也允许"右贴到别人左边"）。
    {
        const SnapResult snapped =
            snapRect(Rect{40, 130, 62, 50}, windows, 8);
        CHECK(snapped.snappedRight);
        CHECK(snapped.rect.right() == 100);
    }

    // 上边缘靠近窗口底边 → 上吸附。
    {
        const SnapResult snapped =
            snapRect(Rect{120, 253, 60, 40}, windows, 8);
        CHECK(snapped.snappedTop);
        CHECK(snapped.rect.y == 250);
    }

    // 距离超出阈值 → 不吸附、原样返回。
    {
        const SnapResult snapped =
            snapRect(Rect{140, 130, 50, 50}, windows, 8);
        CHECK(!snapped.snapped());
        // 花括号初始化列表在宏实参里需要额外括号。
        CHECK((snapped.rect == Rect{140, 130, 50, 50}));
    }

    // 阈值 0 / 无候选 → 不吸附。
    {
        CHECK((!snapRect(Rect{103, 130, 50, 50}, windows, 0).snapped()));
        const std::vector<Rect> noCandidates;
        CHECK((!snapRect(Rect{103, 130, 50, 50}, noCandidates, 8).snapped()));
    }

    // 两边都命中且右侧更近 → 只动右边，不把宽度拉变形。
    {
        // 左 104（距 100 为 4），右 297（距 300 为 3）。
        const SnapResult snapped =
            snapRect(Rect{104, 130, 193, 50}, windows, 8);
        CHECK(snapped.snappedRight);
        CHECK(!snapped.snappedLeft);
        CHECK(snapped.rect.x == 104);
        CHECK(snapped.rect.right() == 300);
    }

    // 同时吸附横向与纵向（贴到窗口的左上角外侧）。
    {
        const SnapResult snapped =
            snapRect(Rect{102, 98, 40, 40}, windows, 8);
        CHECK(snapped.snappedLeft);
        CHECK(snapped.snappedTop);
        CHECK(snapped.rect.x == 100);
        CHECK(snapped.rect.y == 100);
    }

    // 多个候选取最近边缘：命中矩形 A 右边(150) 与 B 左边(160)，
    // 拖拽点 158 距 160 更近 → 吸附到 160。
    {
        const std::vector<Rect> two{Rect{0, 0, 150, 100}, Rect{160, 0, 100, 100}};
        const SnapResult snapped =
            snapRect(Rect{158, 20, 30, 30}, two, 8);
        CHECK(snapped.snappedLeft);
        CHECK(snapped.rect.x == 160);
    }
}

void testAnnotation() {
    using namespace jie2::core;

    // 拒绝点不足的标注（画不出任何东西）。
    {
        AnnotationDocument doc;
        Annotation empty;
        empty.tool = AnnotationTool::Rect;
        CHECK(!doc.add(empty));
        Annotation onePoint;
        onePoint.points.push_back(Point{5, 5});
        CHECK(!doc.add(onePoint));
        CHECK(doc.empty());
        CHECK(!doc.undo());
        CHECK(!doc.redo());
    }

    // 添加 → 撤销 → 重做 的完整回环。
    {
        AnnotationDocument doc;
        Annotation rect;
        rect.tool = AnnotationTool::Rect;
        rect.points = {Point{0, 0}, Point{10, 10}};
        CHECK(doc.add(rect));
        CHECK(doc.size() == 1);

        Annotation arrow;
        arrow.tool = AnnotationTool::Arrow;
        arrow.points = {Point{20, 20}, Point{40, 40}};
        CHECK(doc.add(arrow));
        CHECK(doc.size() == 2);

        CHECK(doc.undo());   // 撤销 arrow
        CHECK(doc.size() == 1);
        CHECK(doc.items().back().tool == AnnotationTool::Rect);
        CHECK(doc.redo());   // 重做 arrow
        CHECK(doc.size() == 2);
        CHECK(doc.items().back().tool == AnnotationTool::Arrow);

        // 撤销后新添加 → redo 栈清空（标准编辑器语义）。
        CHECK(doc.undo());
        CHECK(doc.canRedo());
        Annotation marker;
        marker.tool = AnnotationTool::Marker;
        marker.points = {Point{1, 1}, Point{2, 2}};
        CHECK(doc.add(marker));
        CHECK(!doc.canRedo());
    }

    // boundingRect 正确计算（含负坐标方向的点序）。
    {
        Annotation line;
        line.points = {Point{30, 40}, Point{10, 20}, Point{20, 60}};
        const Rect bounds = line.boundingRect();
        CHECK(bounds.x == 10);
        CHECK(bounds.y == 20);
        CHECK(bounds.width == 20);
        CHECK(bounds.height == 40);
    }

    // pixelOpItems 只返回马赛克/模糊，且保持顺序。
    {
        AnnotationDocument doc;
        auto push = [&doc](AnnotationTool tool, int x) {
            Annotation item;
            item.tool = tool;
            item.points = {Point{x, x}, Point{x + 5, x + 5}};
            doc.add(std::move(item));
        };
        push(AnnotationTool::Rect, 0);
        push(AnnotationTool::Mosaic, 10);
        push(AnnotationTool::Arrow, 20);
        push(AnnotationTool::Blur, 30);
        const auto ops = doc.pixelOpItems();
        CHECK(ops.size() == 2);
        CHECK(ops[0]->tool == AnnotationTool::Mosaic);
        CHECK(ops[1]->tool == AnnotationTool::Blur);
    }

    // clear 清空一切。
    {
        AnnotationDocument doc;
        Annotation item;
        item.points = {Point{0, 0}, Point{1, 1}};
        doc.add(item);
        doc.undo();
        doc.clear();
        CHECK(doc.empty());
        CHECK(!doc.canUndo());
        CHECK(!doc.canRedo());
    }

    // 文字与序号：单点即可，文字不允许为空。
    {
        AnnotationDocument doc;
        Annotation text;
        text.tool = AnnotationTool::Text;
        text.points = {Point{10, 10}};
        CHECK(!doc.add(text));  // 无文字内容 → 拒绝

        text.text = "hello";
        CHECK(doc.add(text));
        CHECK(doc.size() == 1);

        Annotation number;
        number.tool = AnnotationTool::Number;
        number.points = {Point{20, 20}};
        CHECK(doc.add(number));  // 单点序号可接受
        CHECK(doc.size() == 2);

        // 两点形状仍然要求两点。
        Annotation rect;
        rect.tool = AnnotationTool::Rect;
        rect.points = {Point{0, 0}};
        CHECK(!doc.add(rect));
    }

    // 序号自动编号：基于当前可见数量；撤销后编号重用。
    {
        AnnotationDocument doc;
        CHECK(doc.nextNumberLabel() == "1");
        Annotation n1;
        n1.tool = AnnotationTool::Number;
        n1.points = {Point{0, 0}};
        doc.add(n1);
        CHECK(doc.nextNumberLabel() == "2");
        Annotation n2;
        n2.tool = AnnotationTool::Number;
        n2.points = {Point{5, 5}};
        doc.add(n2);
        CHECK(doc.nextNumberLabel() == "3");
        doc.undo();  // 撤掉 n2
        CHECK(doc.nextNumberLabel() == "2");  // 编号重用（画面上只剩 1 个）
    }

    // 橡皮擦：按包围盒相交整条删除；不相交的不受影响。
    {
        AnnotationDocument doc;
        auto push = [&doc](int x, int y, int w, int h) {
            Annotation item;
            item.tool = AnnotationTool::Rect;
            item.points = {Point{x, y}, Point{x + w, y + h}};
            doc.add(std::move(item));
        };
        push(10, 10, 50, 50);    // A: (10,10)-(60,60)
        push(200, 200, 40, 40);  // B: 远离
        push(100, 10, 30, 30);   // C: (100,10)-(130,40)
        CHECK(doc.size() == 3);

        // 擦 (15,15)-(25,25)：只与 A 相交
        CHECK(doc.eraseIntersecting(Rect{15, 15, 10, 10}) == 1);
        CHECK(doc.size() == 2);

        // 擦远离所有标注的区域：无变化
        CHECK(doc.eraseIntersecting(Rect{500, 500, 10, 10}) == 0);
        CHECK(doc.size() == 2);

        // 擦 (105,15)-(115,25)：与 C 相交；B 仍保留
        CHECK(doc.eraseIntersecting(Rect{105, 15, 10, 10}) == 1);
        CHECK(doc.size() == 1);
        CHECK(doc.items().front().boundingRect().x == 200);

        // 空区域不误删
        CHECK(doc.eraseIntersecting(Rect{}) == 0);
        CHECK(doc.size() == 1);
    }

    // 虚线 / 圆角是样式字段，不影响模型行为（渲染层消费）。
    {
        AnnotationDocument doc;
        Annotation dashedRect;
        dashedRect.tool = AnnotationTool::Rect;
        dashedRect.style.dashed = true;
        dashedRect.style.cornerRadius = 16;
        dashedRect.points = {Point{0, 0}, Point{10, 10}};
        CHECK(doc.add(dashedRect));
        CHECK(doc.items().front().style.dashed);
        CHECK(doc.items().front().style.cornerRadius == 16);
    }

    // 椭圆马赛克/模糊的最小点数要求与矩形一致（两点）。
    {
        AnnotationDocument doc;
        Annotation ellipseMosaic;
        ellipseMosaic.tool = AnnotationTool::Mosaic;
        ellipseMosaic.style.ellipseShape = true;
        ellipseMosaic.points = {Point{0, 0}};
        CHECK(!doc.add(ellipseMosaic));  // 只有一点 → 拒绝
        ellipseMosaic.points.push_back(Point{10, 10});
        CHECK(doc.add(ellipseMosaic));
        CHECK(doc.items().front().style.ellipseShape);
    }
}

void testEllipseMaskOps() {
    using namespace jie2::core;

    // 构造红蓝棋盘（4x4 块），对椭圆区域做马赛克：
    //   区域外：原样（棋盘色）
    //   椭圆内：被像素化（出现混色）
    //   椭圆外但矩形内（四角）：原样
    {
        ImageBuffer image(64, 64, PixelFormat::RGBA8888);
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                const bool red = ((x / 4 + y / 4) % 2 == 0);
                std::uint8_t* p = image.scanline(y) + static_cast<std::size_t>(x) * 4;
                p[0] = red ? 255 : 0;
                p[1] = 0;
                p[2] = red ? 0 : 255;
                p[3] = 255;
            }
        }
        // 椭圆区域：以 (32,32) 为中心，宽 48 高 32 的外接矩形。
        const Rect region{8, 16, 48, 32};
        const ImageBuffer result = pixelate(image, region, 8, /*ellipseMask=*/true);

        // 椭圆中心：被处理（不再是纯红/纯蓝）
        const Rgba center = result.pixel(32, 32);
        const bool centerNeutral =
            center.r > 60 && center.r < 200 && center.b > 60 && center.b < 200;
        CHECK(centerNeutral);

        // 矩形四角（椭圆外）：保持原棋盘色。
        // (10,18) 距中心 dx=-21.5, dy=-13.5 → 0.86^2 + 0.65^2 > 1 → 椭圆外。
        const Rgba corner = result.pixel(10, 18);
        const bool cornerIsBoard =
            (corner.r == 255 && corner.b == 0) || (corner.r == 0 && corner.b == 255);
        CHECK(cornerIsBoard);

        // 区域外的像素完全不受影响（抽样）。
        const Rgba outside = result.pixel(2, 2);
        const bool outsideIsBoard =
            (outside.r == 255 && outside.b == 0) ||
            (outside.r == 0 && outside.b == 255);
        CHECK(outsideIsBoard);
    }

    // 椭圆模糊：中心应被平滑、四角保持原样。
    {
        ImageBuffer image(64, 64, PixelFormat::RGBA8888);
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                const bool red = ((x / 2) % 2 == 0);
                std::uint8_t* p = image.scanline(y) + static_cast<std::size_t>(x) * 4;
                p[0] = red ? 255 : 0;
                p[1] = 0;
                p[2] = red ? 0 : 255;
                p[3] = 255;
            }
        }
        const Rect region{8, 16, 48, 32};
        const ImageBuffer result = boxBlur(image, region, 9, /*ellipseMask=*/true);
        const Rgba center = result.pixel(32, 32);
        const bool centerSmooth = center.r > 20 && center.r < 235;
        CHECK(centerSmooth);
        const Rgba corner = result.pixel(10, 18);
        const bool cornerIsBoard =
            (corner.r == 255 && corner.b == 0) || (corner.r == 0 && corner.b == 255);
        CHECK(cornerIsBoard);
    }
}

} // namespace

int main() {
    testGeometry();
    testImageBuffer();
    testImageOps();
    testConfig();
    testCapability();
    testImageOpsDecorations();
    testCaptureHistory();
    testSnap();
    testAnnotation();
    testEllipseMaskOps();

    if (g_failures == 0) {
        std::printf("all core tests passed\n");
        return 0;
    }
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
}
