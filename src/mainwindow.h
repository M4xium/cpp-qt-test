#pragma once

#include "processingsettings.h"

#include <QMainWindow>
#include <QThread>
#include <QTimer>

class QCheckBox;
class QCloseEvent;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class FileProcessor;

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void buildInterface();
    void connectWorker();
    void restoreSettings();
    void saveSettings() const;
    bool collectSettings(ProcessingSettings *settings);
    QStringList normalizedFilters() const;
    void startSession();
    void startCycle();
    void stopSession();
    void togglePause();
    void finishSession(const QString &statusText);
    void setConfigurationEnabled(bool enabled);
    void appendLog(const QString &message, bool isError = false);
    void shutdownWorker();
    static QString formatBytes(qint64 bytes);

    QLineEdit *m_inputDirectoryEdit = nullptr;
    QLineEdit *m_outputDirectoryEdit = nullptr;
    QLineEdit *m_filterEdit = nullptr;
    QCheckBox *m_deleteInputsCheck = nullptr;
    QComboBox *m_collisionCombo = nullptr;
    QComboBox *m_modeCombo = nullptr;
    QSpinBox *m_intervalSpin = nullptr;
    QLineEdit *m_keyEdit = nullptr;
    QPushButton *m_inputBrowseButton = nullptr;
    QPushButton *m_outputBrowseButton = nullptr;
    QPushButton *m_startButton = nullptr;
    QPushButton *m_pauseButton = nullptr;
    QPushButton *m_stopButton = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_currentFileLabel = nullptr;
    QLabel *m_byteProgressLabel = nullptr;
    QLabel *m_batchLabel = nullptr;
    QProgressBar *m_progressBar = nullptr;
    QPlainTextEdit *m_logEdit = nullptr;

    FileProcessor *m_worker = nullptr;
    QThread m_workerThread;
    QTimer m_pollTimer;
    ProcessingSettings m_activeSettings;
    bool m_sessionActive = false;
    bool m_cycleActive = false;
    bool m_firstCycle = true;
    bool m_stopRequestedByUser = false;
    bool m_pauseRequestedByUser = false;
    bool m_shuttingDown = false;
    int m_totalSucceeded = 0;
    int m_totalFailed = 0;
};
