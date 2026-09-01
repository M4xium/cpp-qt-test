#include "fileprocessor.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

namespace
{
QByteArray xorData(QByteArray data, const QByteArray &key)
{
    for (qsizetype i = 0; i < data.size(); ++i) {
        data[i] = static_cast<char>(static_cast<unsigned char>(data.at(i))
                                    ^ static_cast<unsigned char>(key.at(i % key.size())));
    }
    return data;
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

ProcessingSettings baseSettings(const QTemporaryDir &input, const QTemporaryDir &output)
{
    ProcessingSettings settings;
    settings.inputDirectory = input.path();
    settings.outputDirectory = output.path();
    settings.nameFilters = {QStringLiteral("*.bin")};
    settings.xorKey = QByteArray::fromHex("1234567890ABCDEF");
    return settings;
}
}

class FileProcessorTest final : public QObject
{
    Q_OBJECT

private slots:
    void transformsEveryByteAndRepeatsKey();
    void addsCounterAndSkipsUnchangedInputDuringSession();
    void deletesInputOnlyAfterSuccess();
    void pausesAndResumesInWorkerThread();
    void stopsCleanlyWhilePaused();
};

void FileProcessorTest::transformsEveryByteAndRepeatsKey()
{
    QTemporaryDir input;
    QTemporaryDir output;
    QVERIFY(input.isValid());
    QVERIFY(output.isValid());

    const QByteArray original = QByteArray::fromHex(
        "000102030405060708090A0B0C0D0E0F101112131415161718");
    const QString inputPath = QDir(input.path()).filePath(QStringLiteral("sample.bin"));
    QVERIFY(writeFile(inputPath, original));

    ProcessingSettings settings = baseSettings(input, output);
    FileProcessor processor;
    QSignalSpy completed(&processor, &FileProcessor::batchFinished);
    processor.prepareCycle();
    processor.processBatch(settings, true);

    QCOMPARE(completed.count(), 1);
    const QString outputPath = QDir(output.path()).filePath(QStringLiteral("sample.bin"));
    QCOMPARE(readFile(outputPath), xorData(original, settings.xorKey));
    QCOMPARE(readFile(inputPath), original);
}

void FileProcessorTest::addsCounterAndSkipsUnchangedInputDuringSession()
{
    QTemporaryDir input;
    QTemporaryDir output;
    QVERIFY(input.isValid());
    QVERIFY(output.isValid());

    const QString inputPath = QDir(input.path()).filePath(QStringLiteral("sample.bin"));
    const QString occupiedPath = QDir(output.path()).filePath(QStringLiteral("sample.bin"));
    QVERIFY(writeFile(inputPath, QByteArray("first payload")));
    QVERIFY(writeFile(occupiedPath, QByteArray("keep me")));

    ProcessingSettings settings = baseSettings(input, output);
    settings.existingFileAction = ExistingFileAction::AddCounter;
    FileProcessor processor;

    processor.prepareCycle();
    processor.processBatch(settings, true);
    const QString firstResult = QDir(output.path()).filePath(QStringLiteral("sample_1.bin"));
    QVERIFY(QFileInfo::exists(firstResult));
    QCOMPARE(readFile(occupiedPath), QByteArray("keep me"));

    processor.prepareCycle();
    processor.processBatch(settings, false);
    QVERIFY(!QFileInfo::exists(QDir(output.path()).filePath(QStringLiteral("sample_2.bin"))));

    QVERIFY(writeFile(inputPath, QByteArray("changed payload with another size")));
    processor.prepareCycle();
    processor.processBatch(settings, false);
    QVERIFY(QFileInfo::exists(QDir(output.path()).filePath(QStringLiteral("sample_2.bin"))));
}

void FileProcessorTest::deletesInputOnlyAfterSuccess()
{
    QTemporaryDir input;
    QTemporaryDir output;
    QVERIFY(input.isValid());
    QVERIFY(output.isValid());

    const QString inputPath = QDir(input.path()).filePath(QStringLiteral("remove.bin"));
    QVERIFY(writeFile(inputPath, QByteArray("payload")));

    ProcessingSettings settings = baseSettings(input, output);
    settings.deleteInputFiles = true;
    FileProcessor processor;
    processor.prepareCycle();
    processor.processBatch(settings, true);

    QVERIFY(!QFileInfo::exists(inputPath));
    QVERIFY(QFileInfo::exists(QDir(output.path()).filePath(QStringLiteral("remove.bin"))));
}

void FileProcessorTest::pausesAndResumesInWorkerThread()
{
    QTemporaryDir input;
    QTemporaryDir output;
    QVERIFY(input.isValid());
    QVERIFY(output.isValid());

    const QByteArray original(2 * 1024 * 1024, static_cast<char>(0xA5));
    const QString inputPath = QDir(input.path()).filePath(QStringLiteral("pause.bin"));
    QVERIFY(writeFile(inputPath, original));
    const ProcessingSettings settings = baseSettings(input, output);

    QThread thread;
    auto *processor = new FileProcessor;
    processor->moveToThread(&thread);
    connect(&thread, &QThread::finished, processor, &QObject::deleteLater);
    QSignalSpy pauseState(processor, &FileProcessor::pauseStateChanged);
    QSignalSpy completed(processor, &FileProcessor::batchFinished);
    thread.start();

    processor->prepareCycle();
    processor->requestPause();
    QMetaObject::invokeMethod(processor, [processor, settings] {
        processor->processBatch(settings, true);
    }, Qt::QueuedConnection);

    const bool pauseWasObserved = !pauseState.isEmpty() || pauseState.wait(5000);
    processor->requestResume();
    const bool completionWasObserved = !completed.isEmpty() || completed.wait(10000);
    processor->requestStop();
    thread.quit();
    thread.wait();

    QVERIFY(pauseWasObserved);
    QVERIFY(completionWasObserved);
    const QString resultPath = QDir(output.path()).filePath(QStringLiteral("pause.bin"));
    QCOMPARE(readFile(resultPath), xorData(original, settings.xorKey));
}

void FileProcessorTest::stopsCleanlyWhilePaused()
{
    QTemporaryDir input;
    QTemporaryDir output;
    QVERIFY(input.isValid());
    QVERIFY(output.isValid());

    const QByteArray original(1024 * 1024, static_cast<char>(0x5A));
    const QString inputPath = QDir(input.path()).filePath(QStringLiteral("stop.bin"));
    QVERIFY(writeFile(inputPath, original));
    const ProcessingSettings settings = baseSettings(input, output);

    QThread thread;
    auto *processor = new FileProcessor;
    processor->moveToThread(&thread);
    connect(&thread, &QThread::finished, processor, &QObject::deleteLater);
    QSignalSpy pauseState(processor, &FileProcessor::pauseStateChanged);
    QSignalSpy completed(processor, &FileProcessor::batchFinished);
    thread.start();

    processor->prepareCycle();
    processor->requestPause();
    QMetaObject::invokeMethod(processor, [processor, settings] {
        processor->processBatch(settings, true);
    }, Qt::QueuedConnection);

    const bool pauseWasObserved = !pauseState.isEmpty() || pauseState.wait(5000);
    processor->requestStop();
    const bool completionWasObserved = !completed.isEmpty() || completed.wait(5000);
    thread.quit();
    thread.wait();

    QVERIFY(pauseWasObserved);
    QVERIFY(completionWasObserved);
    QVERIFY(completed.last().at(3).toBool());
    QVERIFY(!QFileInfo::exists(QDir(output.path()).filePath(QStringLiteral("stop.bin"))));
    QCOMPARE(readFile(inputPath), original);
}

QTEST_GUILESS_MAIN(FileProcessorTest)

#include "tst_fileprocessor.moc"
