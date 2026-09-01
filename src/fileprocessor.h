#pragma once

#include "processingsettings.h"

#include <QMutex>
#include <QObject>
#include <QSet>
#include <QWaitCondition>

class FileProcessor final : public QObject
{
    Q_OBJECT

public:
    explicit FileProcessor(QObject *parent = nullptr);

    // These four methods are thread-safe and may be called from the GUI thread.
    void prepareCycle();
    void requestPause();
    void requestResume();
    void requestStop();

    // Called in the worker thread.
    void processBatch(const ProcessingSettings &settings, bool newSession);

signals:
    void batchStarted(int fileCount, int skippedCount);
    void fileStarted(const QString &sourcePath, int index, int total);
    void fileProgress(const QString &sourcePath, qint64 processedBytes, qint64 totalBytes);
    void fileFinished(const QString &sourcePath, const QString &outputPath, bool success);
    void batchFinished(int succeeded, int failed, int skipped, bool stopped);
    void pauseStateChanged(bool paused);
    void warningOccurred(const QString &message);
    void errorOccurred(const QString &message);

private:
    enum class FileResult
    {
        Succeeded,
        Failed,
        Stopped
    };

    bool waitUntilRunnable();
    bool stopWasRequested() const;
    FileResult processFile(const QString &sourcePath,
                           const ProcessingSettings &settings,
                           QString *createdOutputPath);
    QString makeOutputPath(const QString &sourcePath,
                           const ProcessingSettings &settings) const;
    static QString fingerprint(const QString &path);
    static bool pathsReferToSameFile(const QString &first, const QString &second);

    mutable QMutex m_controlMutex;
    QWaitCondition m_pauseCondition;
    bool m_pauseRequested = false;
    bool m_stopRequested = false;
    QSet<QString> m_processedFingerprints;
};
