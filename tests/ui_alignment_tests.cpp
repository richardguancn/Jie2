#include <QtTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QMenu>
#include <QScreen>
#include <QPainter>
#include "ui/clothesline.h"
#include "ui/pin_window.h"
#include "ui/selection_overlay.h"

namespace jie2::ui {
class UiAlignmentTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        const QString folder = qEnvironmentVariable("JIE2_UI_ARTIFACTS");
        if (!folder.isEmpty()) QVERIFY(QDir().mkpath(folder));
    }
    void toolbarFits_data() {
        QTest::addColumn<int>("width");
        QTest::addColumn<int>("height");
        QTest::addColumn<bool>("dark");
        QTest::newRow("desktop-dark") << 1280 << 800 << true;
        QTest::newRow("small-light") << 480 << 320 << false;
        QTest::newRow("narrow-dark") << 320 << 480 << true;
    }
    void toolbarFits() {
        QFETCH(int, width); QFETCH(int, height); QFETCH(bool, dark);
        QPixmap image(width, height); image.fill(QColor("#4f647a"));
        SelectionOverlay::Options options; options.darkTheme = dark;
        SelectionOverlay overlay(image, 1, nullptr, options);
        overlay.resize(width, height);
        overlay.frozenLogicalRect_ = overlay.rect();
        overlay.hasSelection_ = true;
        for (const QRect selection : {QRect(1, 1, 80, 50),
                                      QRect(width - 100, height - 70, 90, 60)}) {
            overlay.selectionRect_ = selection;
            const QRect bar = overlay.toolbarRect(selection);
            QVERIFY(overlay.rect().contains(bar.adjusted(-4, -4, 4, 4)));
            for (int i = 0; i < int(overlay.toolbarItems().size()); ++i) {
                const QRect button = overlay.toolbarButtonRect(selection, i);
                QVERIFY(bar.contains(button));
                int hit = -1;
                overlay.toolbarHitTest(button.center(), &hit);
                QCOMPARE(hit, i);
                for (int j = 0; j < i; ++j)
                    QVERIFY(!button.intersects(overlay.toolbarButtonRect(selection, j)));
            }
        }
        overlay.show();
        const QString folder = qEnvironmentVariable("JIE2_UI_ARTIFACTS");
        if (!folder.isEmpty()) {
            QDir().mkpath(folder);
            QVERIFY(overlay.grab().save(folder + QString("/toolbar-%1-%2.png").arg(width).arg(dark)));
        }
    }
    void annotateExistingImage() {
        QPixmap image(240, 160); image.fill(Qt::white);
        SelectionOverlay::Options options; options.selectImage = true;
        SelectionOverlay overlay(image, 1, nullptr, options);
        overlay.show();
        QCOMPARE(overlay.currentSelection().size(), QSize(240, 160));
        const QPoint origin = overlay.currentSelection().topLeft();
        QTest::mousePress(&overlay, Qt::LeftButton, Qt::NoModifier, origin + QPoint(20, 20));
        QTest::mouseMove(&overlay, origin + QPoint(100, 80));
        QTest::mouseRelease(&overlay, Qt::LeftButton, Qt::NoModifier, origin + QPoint(100, 80));
        QCOMPARE(overlay.annotations_.size(), size_t(1));
        overlay.performUndo(); QCOMPARE(overlay.annotations_.size(), size_t(0));
        overlay.performRedo(); QCOMPARE(overlay.annotations_.size(), size_t(1));
        QTest::keyClick(&overlay, Qt::Key_Return);
        QVERIFY(overlay.outcome().accepted);
        QCOMPARE(overlay.outcome().outputImage.size(), QSize(240, 160));
        const QImage result = overlay.outcome().outputImage;
        QVERIFY(result.pixelColor(50, 20) != QColor(Qt::white));
        QCOMPARE(result.pixelColor(150, 100), QColor(Qt::white));
    }
    void toolbarDoubleClickDoesNotSave() {
        QPixmap image(240, 160); image.fill(Qt::white);
        SelectionOverlay::Options options; options.selectImage = true;
        SelectionOverlay overlay(image, 1, nullptr, options);
        overlay.show();
        const QPoint tool = overlay.toolbarButtonRect(overlay.currentSelection(), 1).center();
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, tool);
        QCOMPARE(overlay.currentTool_, core::AnnotationTool::Arrow);
        QTest::mouseDClick(&overlay, Qt::LeftButton, Qt::NoModifier, tool);
        QVERIFY(!overlay.outcome().accepted);
        QTest::keyClick(&overlay, Qt::Key_Escape);
        QVERIFY(!overlay.outcome().accepted);
        QVERIFY(overlay.outcome().outputImage.isNull());
    }
    void largeImageKeepsResolution() {
        QPixmap image(1600, 1200); image.fill(Qt::yellow);
        SelectionOverlay::Options options; options.selectImage = true;
        SelectionOverlay overlay(image, 1, nullptr, options);
        QCOMPARE(overlay.selectionToPixelRegion(overlay.currentSelection()).size(), QSize(1600, 1200));
        overlay.commit(true);
        QCOMPARE(overlay.outcome().outputImage.size(), QSize(1600, 1200));
    }
    void clotheslineGestures() {
        QTemporaryDir temp;
        QImage image(640, 480, QImage::Format_RGB32); image.fill(Qt::cyan);
        const QString path = temp.filePath("sample.png"); QVERIFY(image.save(path));
        Clothesline line;
        line.setEntries({{path.toStdString(), "20261016", 640, 480}});
        line.revealForTest(QRect(0, 0, 800, 600));
        QTest::qWait(600);
        const QPoint center = line.cardRectF(0).center().toPoint();
        QSignalSpy copied(&line, &Clothesline::copyRequested);
        QSignalSpy opened(&line, &Clothesline::openRequested);
        QSignalSpy annotated(&line, &Clothesline::annotateRequested);
        QTest::mouseClick(&line, Qt::LeftButton, Qt::NoModifier, center);
        QCOMPARE(copied.count(), 1);
        QTest::mouseDClick(&line, Qt::LeftButton, Qt::NoModifier, center);
        QTest::mouseRelease(&line, Qt::LeftButton, Qt::NoModifier, center);
        QCOMPARE(opened.count(), 1); QCOMPARE(copied.count(), 1);
        QTest::mousePress(&line, Qt::LeftButton, Qt::NoModifier, center);
        QTest::qWait(520);
        QCOMPARE(annotated.count(), 1);
        QTest::mouseRelease(&line, Qt::LeftButton, Qt::NoModifier, center);
        QCOMPARE(copied.count(), 1);
        QCOMPARE(annotated.at(0).at(0).toString(), path);
        // 释放、隐藏、数据更新都必须取消尚未到期的长按。
        QTest::mousePress(&line, Qt::LeftButton, Qt::NoModifier, center);
        line.setEntries({{path.toStdString(), "20261016", 640, 480}});
        QTest::qWait(500); QCOMPARE(annotated.count(), 1);
        QTest::mousePress(&line, Qt::LeftButton, Qt::NoModifier, center);
        line.hideNow(); QTest::qWait(500); QCOMPARE(annotated.count(), 1);
        QVERIFY(QFileInfo::exists(path));
    }
    void cropHandlesResizeAndMove() {
        QPixmap image(640, 480); image.fill(Qt::white);
        SelectionOverlay overlay(image, 1, nullptr);
        overlay.hasSelection_ = true; overlay.selectionRect_ = QRect(100, 100, 200, 160);
        overlay.show();
        // 右边中点扩大宽度，不改变高度。
        QPoint handle = overlay.cropHandles()[3];
        QTest::mousePress(&overlay, Qt::LeftButton, Qt::NoModifier, handle);
        QTest::mouseMove(&overlay, handle + QPoint(40, 0));
        QTest::mouseRelease(&overlay, Qt::LeftButton, Qt::NoModifier, handle + QPoint(40, 0));
        QCOMPARE(overlay.currentSelection(), QRect(100, 100, 240, 160));
        QTest::mousePress(&overlay, Qt::LeftButton, Qt::NoModifier, QPoint(180, 180));
        QTest::mouseMove(&overlay, QPoint(210, 210));
        QTest::mouseRelease(&overlay, Qt::LeftButton, Qt::NoModifier, QPoint(210, 210));
        QCOMPARE(overlay.currentSelection(), QRect(130, 130, 240, 160));
        QCOMPARE(overlay.annotations_.size(), size_t(0));
    }
    void actionButtonsAreDistinct() {
        for (const auto action : {SelectionOverlay::Action::SaveFile,
                                 SelectionOverlay::Action::Copy, SelectionOverlay::Action::Pin}) {
            QPixmap image(240, 160); image.fill(Qt::white);
            SelectionOverlay::Options options; options.selectImage = true;
            SelectionOverlay overlay(image, 1, nullptr, options);
            overlay.show();
            const int index = action == SelectionOverlay::Action::SaveFile ? 11 :
                              action == SelectionOverlay::Action::Copy ? 12 : 10;
            QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier,
                              overlay.toolbarButtonRect(overlay.currentSelection(), index).center());
            QVERIFY(overlay.outcome().accepted);
            QCOMPARE(overlay.outcome().action, action);
            QCOMPARE(overlay.outcome().outputImage.size(), QSize(240, 160));
        }
    }
    void clotheslineWindowDoesNotDrift() {
        QTemporaryDir temp;
        QImage image(400, 240, QImage::Format_RGB32); image.fill(Qt::darkBlue);
        const QString path = temp.filePath("sample.png"); QVERIFY(image.save(path));
        Clothesline line;
        line.setEntries({{path.toStdString(), "", 400, 240}});
        const QRect screen = QGuiApplication::primaryScreen()->geometry();
        for (int cycle = 0; cycle < 3; ++cycle) {
            line.revealForTest(screen);
            QTest::qWait(80);
            const QPoint position = line.pos();
            const double initialOffset = line.contentOffsetY_;
            bool changed = false;
            for (int frame = 0; frame < 50; ++frame) {
                QTest::qWait(16);
                QCOMPARE(line.pos(), position);
                changed |= qAbs(line.contentOffsetY_ - initialOffset) > 1;
            }
            QVERIFY(changed);
            QTRY_VERIFY(qAbs(line.contentOffsetY_) < 0.5);
            qInfo() << "固定窗口第" << cycle + 1 << "轮：位置" << line.pos() << "内容偏移" << line.contentOffsetY_;
            line.conceal();
            QTRY_VERIFY(!line.isVisible());
            QCOMPARE(line.pos(), position);
        }
    }
    void revealDwellAndLeaveDelay() {
        Clothesline line;
        const QRect area(0, 0, 800, 600);
        line.updateRevealForPointer(QPoint(400, 1), area, 1000);
        line.updateRevealForPointer(QPoint(400, 1), area, 1200);
        QVERIFY(!line.isRevealed());
        line.updateRevealForPointer(QPoint(400, 1), area, 1250);
        QVERIFY(line.isRevealed());
        line.updateRevealForPointer(QPoint(400, 500), area, 1500);
        line.updateRevealForPointer(QPoint(400, 500), area, 1999);
        QVERIFY(line.isRevealed());
        line.updateRevealForPointer(QPoint(400, 1), area, 2000);
        line.updateRevealForPointer(QPoint(400, 500), area, 2100);
        line.updateRevealForPointer(QPoint(400, 500), area, 2600);
        QVERIFY(!line.isRevealed());
    }
    void menuMatchesReference() {
        QTemporaryDir temp;
        const QString path = temp.filePath("sample.png");
        QImage image(240, 160, QImage::Format_RGB32); image.fill(Qt::blue); QVERIFY(image.save(path));
        Clothesline line; line.setManagedDirectory(temp.path());
        auto* menu = line.createContextMenu(path);
        QStringList labels;
        for (const auto* action : menu->actions()) if (!action->isSeparator()) labels << action->text();
        QCOMPARE(labels.size(), 6);
        QCOMPARE(labels.at(0), QStringLiteral("复制"));
        QCOMPARE(labels.at(1), QStringLiteral("打开"));
        QCOMPARE(labels.at(2), QStringLiteral("标注"));
        QVERIFY(labels.at(3).contains(QStringLiteral("显示")));
        QCOMPARE(labels.at(4), QStringLiteral("保存到桌面"));
        QCOMPARE(labels.at(5), QStringLiteral("丢弃"));
        QSignalSpy saved(&line, &Clothesline::saveDesktopRequested);
        menu->actions().at(4)->trigger(); QCOMPARE(saved.count(), 1);
        delete menu;
        line.setManagedDirectory(QString());
        menu = line.createContextMenu(path);
        QCOMPARE(menu->actions().last()->text(), QStringLiteral("从衣架取下（保留文件）"));
        delete menu;
    }
    void referencePreviews() {
        const QString folder = qEnvironmentVariable("JIE2_UI_ARTIFACTS");
        if (folder.isEmpty()) return;
        QDir().mkpath(folder);
        QPixmap wallpaper(598, 366);
        { QPainter painter(&wallpaper); QLinearGradient g(0, 0, 598, 366);
          g.setColorAt(0, QColor("#020536")); g.setColorAt(.6, QColor("#1535aa"));
          g.setColorAt(1, QColor("#040a3f")); painter.fillRect(wallpaper.rect(), g); }
        SelectionOverlay overlay(wallpaper, 1, nullptr);
        overlay.resize(598, 366); overlay.frozenLogicalRect_ = overlay.rect();
        overlay.hasSelection_ = true; overlay.selectionRect_ = QRect(339, 111, 140, 117);
        overlay.show();
        QVERIFY(overlay.grab().save(folder + "/snipaste-aligned.png"));
        QTemporaryDir temp;
        std::vector<core::HistoryEntry> entries;
        const QSize sizes[] = {QSize(600,100),QSize(340,300),QSize(400,260),QSize(600,180),QSize(300,440),QSize(360,400),QSize(320,400)};
        for (int i = 0; i < 7; ++i) {
            QImage image(sizes[i], QImage::Format_RGB32); image.fill(i % 2 ? QColor("#eef1f5") : QColor("#202426"));
            QPainter painter(&image); painter.setPen(i % 2 ? QColor("#63738b") : QColor("#929e95"));
            for (int y = 16; y < image.height() - 10; y += 16) painter.drawLine(14, y, image.width() - 25 - (y % 3) * 10, y);
            painter.end();
            const QString path = temp.filePath(QString::number(i) + ".png"); QVERIFY(image.save(path));
            entries.push_back({path.toStdString(), "", image.width(), image.height()});
        }
        Clothesline line; line.setEntries(entries); line.revealForTest(QRect(0, 0, 1920, 600));
        QTest::qWait(1000);
        QImage preview(1920, 210, QImage::Format_RGB32); preview.fill(QColor("#9bc5dd"));
        QPainter painter(&preview); line.render(&painter); painter.end();
        QVERIFY(preview.save(folder + "/clothesline-aligned.png"));
        line.setManagedDirectory(temp.path());
        auto* menu = line.createContextMenu(QString::fromStdString(entries.front().path));
        menu->ensurePolished(); menu->adjustSize();
        QVERIFY(menu->grab().save(folder + "/clothesline-menu.png"));
        delete menu;

    }
    void copiedBadgeAndRevealBreeze() {
        QTemporaryDir temp;
        QImage image(320, 180, QImage::Format_RGB32); image.fill(QColor("#22252e"));
        const QString path = temp.filePath("sample.png"); QVERIFY(image.save(path));
        Clothesline line;
        line.setEntries({{path.toStdString(), "", 320, 180}, {temp.filePath("other.png").toStdString(), "", 320, 180}});
        line.revealForTest(QRect(0, 0, 800, 600));
        QTRY_VERIFY_WITH_TIMEOUT(qAbs(line.entries_[0].swing.value) > .05, 1400);
        QTRY_VERIFY_WITH_TIMEOUT(qAbs(line.entries_[1].swing.value) > .05, 1400);
        line.showCopied(path);
        QCOMPARE(line.copiedPath_, path);
        const qint64 first = line.copiedUntilMs_;
        QTest::qWait(150);
        line.showCopied(path);
        QVERIFY(line.copiedUntilMs_ > first);
        line.setEntries({{path.toStdString(), "", 320, 180}});
        QCOMPARE(line.copiedPath_, path);
        const QString folder = qEnvironmentVariable("JIE2_UI_ARTIFACTS");
        if (!folder.isEmpty()) {
            QDir().mkpath(folder);
            QVERIFY(line.grab().save(folder + "/copied-feedback.png"));
        }
        QTest::qWait(1800);
        QVERIFY(line.copiedPath_.isEmpty());
        line.showCopied(path);
        line.setEntries({});
        QVERIFY(line.copiedPath_.isEmpty());
        line.hideNow();
    }
    void pinDetail_data() {
        QTest::addColumn<QPoint>("clicked");
        QTest::newRow("off-center") << QPoint(180, 110);
        QTest::newRow("top-left") << QPoint(2, 2);
        QTest::newRow("bottom-right") << QPoint(397, 237);
    }
    void pinDetail() {
        QFETCH(QPoint, clicked);
        QImage image(400, 240, QImage::Format_RGB32);
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) image.setPixelColor(x, y, QColor(x % 256, y % 256, 70));
        PinWindow pin(image); pin.move(400, 300); pin.show();
        QSignalSpy closed(&pin, &PinWindow::closed);
        const QPoint global = pin.mapToGlobal(clicked);
        QTest::mouseDClick(&pin, Qt::LeftButton, Qt::NoModifier, clicked);
        QVERIFY(pin.detailMode_); QCOMPARE(pin.size(), QSize(54, 54));
        QCOMPARE(pin.detailCenter_, QPointF(clicked));
        QCOMPARE(pin.pos() + QPoint(27, 27), global);
        QCOMPARE(pin.detailSourceRect().center(), QPointF(clicked));
        const QImage captured = pin.grab().toImage();
        const qreal dpr = captured.devicePixelRatio();
        const QColor center = captured.pixelColor(qRound(27*dpr), qRound(27*dpr));
        const QColor expected = image.pixelColor(clicked);
        QVERIFY(qAbs(center.red() - expected.red()) <= 2);
        QVERIFY(qAbs(center.green() - expected.green()) <= 2);
        QCOMPARE(closed.count(), 0);
        const QString folder = qEnvironmentVariable("JIE2_UI_ARTIFACTS");
        if (!folder.isEmpty() && clicked == QPoint(180, 110)) {
            QVERIFY(pin.grab().save(folder + "/pin-detail.png"));
        }
        pin.applyScaleFactor(2);
        QCOMPARE(pin.size(), QSize(54, 54));
        QCOMPARE(pin.detailSourceRect().center(), QPointF(clicked));
        QCOMPARE(pin.detailSourceRect().width(), 25.0);
        QTest::mouseDClick(&pin, Qt::LeftButton, Qt::NoModifier, QPoint(27, 27));
        QVERIFY(!pin.detailMode_); QCOMPARE(pin.size(), QSize(800, 480));
        QCOMPARE(pin.pos() + clicked * 2, global);
        QCOMPARE(pin.displayedImage(), image);
        QTest::keyClick(&pin, Qt::Key_Escape);
        QCOMPARE(closed.count(), 1);
    }
    void pinDetailRetinaAndTransforms() {
        QImage image(800, 480, QImage::Format_RGB32); image.fill(Qt::green); image.setDevicePixelRatio(2);
        PinWindow pin(image); pin.show();
        QCOMPARE(pin.size(), QSize(400, 240));
        QTest::mouseDClick(&pin, Qt::LeftButton, Qt::NoModifier, QPoint(100, 80));
        QCOMPARE(pin.detailCenter_, QPointF(200, 160));
        QCOMPARE(pin.detailSourceRect().width(), 100.0);
        pin.rotateClockwise(); QCOMPARE(pin.detailCenter_, QPointF(320, 200));
        pin.rotateCounterClockwise(); QCOMPARE(pin.detailCenter_, QPointF(200, 160));
        pin.flipHorizontal(); QCOMPARE(pin.detailCenter_, QPointF(600, 160));
        pin.flipVertical(); QCOMPARE(pin.detailCenter_, QPointF(600, 320));
        pin.resetTo100(); QVERIFY(!pin.detailMode_);
        QCOMPARE(pin.size(), QSize(400, 240));
        // 原有修饰键缩略图仍可用。
        QTest::mouseDClick(&pin, Qt::LeftButton, Qt::ShiftModifier, QPoint(100, 80));
        QVERIFY(pin.thumbnail_); QVERIFY(!pin.detailMode_);
    }
    void contextMenuKeepsFiles() {
        QTemporaryDir temp;
        QImage image(240, 160, QImage::Format_RGB32); image.fill(Qt::green);
        const QString path = temp.filePath("sample.png"); QVERIFY(image.save(path));
        Clothesline line; line.setEntries({{path.toStdString(), "", 240, 160}});
        line.revealForTest(QRect(0, 0, 800, 600)); QTest::qWait(600);
        QSignalSpy dropped(&line, &Clothesline::entryDropped);
        connect(&line, &Clothesline::discardRequested, &line, &Clothesline::takeDownPath);
        QTimer::singleShot(20, &line, [&] {
            auto* menu = line.findChild<QMenu*>();
            if (menu) {
                menu->setActiveAction(menu->actions().last());
                QTest::keyClick(menu, Qt::Key_Return);
            }
        });
        QTest::mouseClick(&line, Qt::RightButton, Qt::NoModifier, line.cardRectF(0).center().toPoint());
        QTRY_COMPARE(dropped.count(), 1);
        QCOMPARE(line.itemCount(), 0);
        QVERIFY(QFileInfo::exists(path));
    }
};
}
QTEST_MAIN(jie2::ui::UiAlignmentTest)
#include "ui_alignment_tests.moc"
