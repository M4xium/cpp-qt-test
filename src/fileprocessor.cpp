#include "fileprocessor.h"

#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QSaveFile>

#include <algorithm>

namespace
{
constexpr qsizetype kBufferSize = 4 * 1024 * 1024;
constexpr qint64 kProgressUpdateIntervalMs = 100;
}

FileProcessor::FileProcessor(QObject *parent)
    : QObject(parent)
{
}

void FileProcessor::prepareCycle()
{
    QMutexLocker locker(&m_controlMutex);
    m_stopRequested = false;
    m_pauseRequested = false;
    m_pauseCondition.wakeAll();
}

void FileProcessor::requestPause()
{
    QMutexLocker locker(&m_controlMutex);
    if (!m_stopRequested)
        m_pauseRequested = true;
}

void FileProcessor::requestResume()
{
    QMutexLocker locker(&m_controlMutex);
    m_pauseRequested = false;
    m_pauseCondition.wakeAll();
}

void FileProcessor::requestStop()
{
    QMutexLocker locker(&m_controlMutex);
    m_stopRequested = true;
    m_pauseRequested = false;
    m_pauseCondition.wakeAll();
}

bool FileProcessor::waitUntilRunnable()
{
    QMutexLocker locker(&m_controlMutex);
    if (m_stopRequested)
        return false;

    if (!m_pauseRequested)
        return true;

    locker.unlock();
    emit pauseStateChanged(true);
    locker.relock();

    while (m_pauseRequested && !m_stopRequested)
        m_pauseCondition.wait(&m_controlMutex);

    const bool mayContinue = !m_stopRequested;
    locker.unlock();
    emit pauseStateChanged(false);
    return mayContinue;
}

bool FileProcessor::stopWasRequested() const
{
    QMutexLocker locker(&m_controlMutex);
    return m_stopRequested;
}

void FileProcessor::processBatch(const ProcessingSettings &settings, bool newSession)
{
    if (newSession)
        m_processedFingerprints.clear();

    if (settings.xorKey.size() != 8) {
        emit errorOccurred(tr("Ключ XOR должен содержать ровно 8 байт."));
        emit batchFinished(0, 1, 0, false);
        return;
    }

    QDir outputDirectory(settings.outputDirectory);
    if (!outputDirectory.exists() && !QDir().mkpath(outputDirectory.absolutePath())) {
        emit errorOccurred(tr("Не удалось создать папку результатов: %1")
                               .arg(outputDirectory.absolutePath()));
        emit batchFinished(0, 1, 0, false);
        return;
    }

    QStringList matchingFiles;
    QDirIterator iterator(settings.inputDirectory,
                          settings.nameFilters,
                          QDir::Files | QDir::Readable | QDir::NoSymLinks,
                          QDirIterator::NoIteratorFlags);

    while (iterator.hasNext()) {
        if (!waitUntilRunnable()) {
            emit batchFinished(0, 0, 0, true);
            return;
        }
        matchingFiles.append(QFileInfo(iterator.next()).absoluteFilePath());
    }

    std::sort(matchingFiles.begin(), matchingFiles.end(), [](const QString &left, const QString &right) {
        return QString::localeAwareCompare(left, right) < 0;
    });

    QStringList filesToProcess;
    int skipped = 0;
    for (const QString &path : matchingFiles) {
        if (m_processedFingerprints.contains(fingerprint(path))) {
            ++skipped;
        } else {
            filesToProcess.append(path);
        }
    }

    emit batchStarted(filesToProcess.size(), skipped);

    int succeeded = 0;
    int failed = 0;
    for (int index = 0; index < filesToProcess.size(); ++index) {
        if (!waitUntilRunnable())
            break;

        const QString sourcePath = filesToProcess.at(index);
        const QString sourceFingerprint = fingerprint(sourcePath);
        emit fileStarted(sourcePath, index + 1, filesToProcess.size());

        QString outputPath;
        const FileResult result = processFile(sourcePath, settings, &outputPath);
        if (result == FileResult::Stopped)
            break;

        if (result == FileResult::Succeeded) {
            ++succeeded;
            m_processedFingerprints.insert(sourceFingerprint);
            emit fileFinished(sourcePath, outputPath, true);
        } else {
            ++failed;
            emit fileFinished(sourcePath, outputPath, false);
        }
    }

    emit batchFinished(succeeded, failed, skipped, stopWasRequested());
}

FileProcessor::FileResult FileProcessor::processFile(const QString &sourcePath,
                                                     const ProcessingSettings &settings,
                                                     QString *createdOutputPath)
{
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        emit errorOccurred(tr("Не удалось открыть входной файл «%1»: %2")
                               .arg(QDir::toNativeSeparators(sourcePath), source.errorString()));
        return FileResult::Failed;
    }

    const QString outputPath = makeOutputPath(sourcePath, settings);
    if (createdOutputPath)
        *createdOutputPath = outputPath;

    if (pathsReferToSameFile(sourcePath, outputPath)) {
        emit errorOccurred(tr("Входной и выходной файл совпадают: %1")
                               .arg(QDir::toNativeSeparators(sourcePath)));
        return FileResult::Failed;
    }

    QSaveFile output(outputPath);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)) {
        emit errorOccurred(tr("Не удалось открыть выходной файл «%1»: %2")
                               .arg(QDir::toNativeSeparators(outputPath), output.errorString()));
        return FileResult::Failed;
    }

    const qint64 totalBytes = source.size();
    qint64 processedBytes = 0;
    emit fileProgress(sourcePath, 0, totalBytes);

    QByteArray buffer(kBufferSize, Qt::Uninitialized);
    QElapsedTimer progressTimer;
    progressTimer.start();

    for (;;) {
        if (!waitUntilRunnable()) {
            output.cancelWriting();
            return FileResult::Stopped;
        }

        const qint64 bytesRead = source.read(buffer.data(), buffer.size());
        if (bytesRead < 0) {
            emit errorOccurred(tr("Ошибка чтения файла «%1»: %2")
                                   .arg(QDir::toNativeSeparators(sourcePath), source.errorString()));
            output.cancelWriting();
            return FileResult::Failed;
        }
        if (bytesRead == 0)
            break;

        for (qint64 i = 0; i < bytesRead; ++i) {
            const qsizetype keyIndex = static_cast<qsizetype>((processedBytes + i) % 8);
            buffer[static_cast<qsizetype>(i)] =
                static_cast<char>(static_cast<unsigned char>(buffer.at(static_cast<qsizetype>(i)))
                                  ^ static_cast<unsigned char>(settings.xorKey.at(keyIndex)));
        }

        qint64 written = 0;
        while (written < bytesRead) {
            const qint64 part = output.write(buffer.constData() + written, bytesRead - written);
            if (part <= 0) {
                emit errorOccurred(tr("Ошибка записи файла «%1»: %2")
                                       .arg(QDir::toNativeSeparators(outputPath), output.errorString()));
                output.cancelWriting();
                return FileResult::Failed;
            }
            written += part;
        }

        processedBytes += bytesRead;
        if (progressTimer.elapsed() >= kProgressUpdateIntervalMs || processedBytes == totalBytes) {
            emit fileProgress(sourcePath, processedBytes, totalBytes);
            progressTimer.restart();
        }
    }

    // Windows does not allow QFile::remove() while the source handle is open.
    source.close();

    if (!output.commit()) {
        emit errorOccurred(tr("Не удалось завершить запись файла «%1»: %2")
                               .arg(QDir::toNativeSeparators(outputPath), output.errorString()));
        return FileResult::Failed;
    }

    emit fileProgress(sourcePath, processedBytes, totalBytes);

    if (settings.deleteInputFiles && !QFile::remove(sourcePath)) {
        emit warningOccurred(tr("Результат сохранён, но входной файл удалить не удалось: %1")
                                 .arg(QDir::toNativeSeparators(sourcePath)));
    }

    return FileResult::Succeeded;
}

QString FileProcessor::makeOutputPath(const QString &sourcePath,
                                      const ProcessingSettings &settings) const
{
    const QString fileName = QFileInfo(sourcePath).fileName();
    const QDir outputDirectory(settings.outputDirectory);
    const QString initialPath = outputDirectory.filePath(fileName);

    if (settings.existingFileAction == ExistingFileAction::Overwrite
        || !QFileInfo::exists(initialPath)) {
        return initialPath;
    }

    const int lastDot = fileName.lastIndexOf(QLatin1Char('.'));
    const bool hasExtension = lastDot > 0;
    const QString stem = hasExtension ? fileName.left(lastDot) : fileName;
    const QString extension = hasExtension ? fileName.mid(lastDot) : QString();

    for (quint64 counter = 1;; ++counter) {
        const QString candidate = outputDirectory.filePath(
            QStringLiteral("%1_%2%3").arg(stem).arg(counter).arg(extension));
        if (!QFileInfo::exists(candidate))
            return candidate;
    }
}

QString FileProcessor::fingerprint(const QString &path)
{
    const QFileInfo info(path);
    return QStringLiteral("%1|%2|%3")
        .arg(QDir::cleanPath(info.absoluteFilePath()).toCaseFolded())
        .arg(info.size())
        .arg(info.lastModified().toMSecsSinceEpoch());
}

bool FileProcessor::pathsReferToSameFile(const QString &first, const QString &second)
{
    const QString firstPath = QDir::cleanPath(QFileInfo(first).absoluteFilePath());
    const QString secondPath = QDir::cleanPath(QFileInfo(second).absoluteFilePath());
#ifdef Q_OS_WIN
    return firstPath.compare(secondPath, Qt::CaseInsensitive) == 0;
#else
    return firstPath == secondPath;
#endif
}
