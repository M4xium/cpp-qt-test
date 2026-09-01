#include "mainwindow.h"

#include <QTest>

class MainWindowTest final : public QObject
{
    Q_OBJECT

private slots:
    void opensAndClosesCleanly();
};

void MainWindowTest::opensAndClosesCleanly()
{
    MainWindow window;
    window.show();
    QTest::qWait(50);

    QVERIFY(window.isVisible());
    QVERIFY(!window.windowTitle().isEmpty());
    QVERIFY(window.close());
}

QTEST_MAIN(MainWindowTest)

#include "tst_mainwindow.moc"
