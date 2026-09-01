#include "mainwindow.h"

#include "fileprocessor.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSettings>
#include <QSpinBox>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWidget>

namespace
{
constexpr int kProgressMaximum = 1000;

QWidget *pathEditorRow(QLineEdit *edit, QPushButton *button)
{
    auto *container = new QWidget;
    auto *layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(edit, 1);
    layout->addWidget(button);
    return container;
}

QString normalizedAbsolutePath(const QString &path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_worker(new FileProcessor)
{
    buildInterface();
    restoreSettings();
    connectWorker();

    m_worker->moveToThread(&m_workerThread);
    connect(&m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_workerThread.setObjectName(QStringLiteral("file-processing-thread"));
    m_workerThread.start();

    m_pollTimer.setTimerType(Qt::CoarseTimer);
    connect(&m_pollTimer, &QTimer::timeout, this, [this] {
        if (m_sessionActive && !m_cycleActive)
            startCycle();
    });

    setWindowTitle(tr("Обработка файлов — XOR 8 байт"));
    resize(900, 720);
    setMinimumSize(760, 620);
}

MainWindow::~MainWindow()
{
    shutdownWorker();
}

void MainWindow::buildInterface()
{
    auto *central = new QWidget(this);
    auto *rootLayout = new QVBoxLayout(central);
    rootLayout->setSpacing(10);

    auto *description = new QLabel(
        tr("Программа находит файлы по маске и записывает результат циклической операции XOR "
           "с восьмибайтовым ключом. Файлы обрабатываются в фоновом потоке блоками по 4 МиБ."));
    description->setWordWrap(true);
    rootLayout->addWidget(description);

    auto *pathsGroup = new QGroupBox(tr("Файлы"));
    auto *pathsForm = new QFormLayout(pathsGroup);
    pathsForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_inputDirectoryEdit = new QLineEdit;
    m_inputBrowseButton = new QPushButton(tr("Обзор…"));
    pathsForm->addRow(tr("Папка для поиска:"),
                      pathEditorRow(m_inputDirectoryEdit, m_inputBrowseButton));

    m_filterEdit = new QLineEdit;
    m_filterEdit->setPlaceholderText(tr("Например: *.txt; testFile.bin; .dat"));
    pathsForm->addRow(tr("Маска файлов:"), m_filterEdit);

    m_outputDirectoryEdit = new QLineEdit;
    m_outputBrowseButton = new QPushButton(tr("Обзор…"));
    pathsForm->addRow(tr("Папка результатов:"),
                      pathEditorRow(m_outputDirectoryEdit, m_outputBrowseButton));

    m_deleteInputsCheck = new QCheckBox(tr("Удалять входной файл после успешной записи результата"));
    pathsForm->addRow(QString(), m_deleteInputsCheck);

    m_collisionCombo = new QComboBox;
    m_collisionCombo->addItem(tr("Перезаписывать существующий файл"));
    m_collisionCombo->addItem(tr("Добавлять счётчик к имени (file_1.bin)"));
    pathsForm->addRow(tr("Если имя занято:"), m_collisionCombo);
    rootLayout->addWidget(pathsGroup);

    auto *operationGroup = new QGroupBox(tr("Режим и операция"));
    auto *operationForm = new QFormLayout(operationGroup);
    operationForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_modeCombo = new QComboBox;
    m_modeCombo->addItem(tr("Однократный запуск"));
    m_modeCombo->addItem(tr("Периодический поиск файлов"));
    operationForm->addRow(tr("Режим:"), m_modeCombo);

    m_intervalSpin = new QSpinBox;
    m_intervalSpin->setRange(1, 86400);
    m_intervalSpin->setValue(10);
    m_intervalSpin->setSuffix(tr(" с"));
    operationForm->addRow(tr("Период опроса:"), m_intervalSpin);

    m_keyEdit = new QLineEdit;
    m_keyEdit->setMaxLength(16);
    m_keyEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_keyEdit->setPlaceholderText(QStringLiteral("1234567890ABCDEF"));
    m_keyEdit->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[0-9A-Fa-f]{0,16}")), m_keyEdit));
    operationForm->addRow(tr("Ключ XOR (16 hex-символов):"), m_keyEdit);
    rootLayout->addWidget(operationGroup);

    auto *buttonLayout = new QHBoxLayout;
    m_startButton = new QPushButton(tr("Запустить"));
    m_pauseButton = new QPushButton(tr("Приостановить"));
    m_stopButton = new QPushButton(tr("Остановить"));
    m_startButton->setDefault(true);
    m_pauseButton->setEnabled(false);
    m_stopButton->setEnabled(false);
    buttonLayout->addWidget(m_startButton);
    buttonLayout->addWidget(m_pauseButton);
    buttonLayout->addWidget(m_stopButton);
    buttonLayout->addStretch();
    rootLayout->addLayout(buttonLayout);

    auto *statusGroup = new QGroupBox(tr("Выполнение"));
    auto *statusLayout = new QVBoxLayout(statusGroup);
    m_statusLabel = new QLabel(tr("Готово к запуску"));
    m_currentFileLabel = new QLabel(tr("Файл: —"));
    m_currentFileLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_batchLabel = new QLabel(tr("Файлы: —"));
    m_progressBar = new QProgressBar;
    m_progressBar->setRange(0, kProgressMaximum);
    m_progressBar->setValue(0);
    m_progressBar->setFormat(QStringLiteral("%p%"));
    m_byteProgressLabel = new QLabel(tr("0 Б / 0 Б"));
    statusLayout->addWidget(m_statusLabel);
    statusLayout->addWidget(m_currentFileLabel);
    statusLayout->addWidget(m_batchLabel);
    statusLayout->addWidget(m_progressBar);
    statusLayout->addWidget(m_byteProgressLabel);
    rootLayout->addWidget(statusGroup);

    m_logEdit = new QPlainTextEdit;
    m_logEdit->setReadOnly(true);
    m_logEdit->setMaximumBlockCount(3000);
    m_logEdit->setPlaceholderText(tr("Здесь появится журнал обработки…"));
    rootLayout->addWidget(m_logEdit, 1);

    setCentralWidget(central);
    statusBar()->showMessage(tr("Настройте параметры и нажмите «Запустить»"));

    connect(m_inputBrowseButton, &QPushButton::clicked, this, [this] {
        const QString directory = QFileDialog::getExistingDirectory(
            this, tr("Выберите папку для поиска"), m_inputDirectoryEdit->text());
        if (!directory.isEmpty())
            m_inputDirectoryEdit->setText(QDir::toNativeSeparators(directory));
    });
    connect(m_outputBrowseButton, &QPushButton::clicked, this, [this] {
        const QString directory = QFileDialog::getExistingDirectory(
            this, tr("Выберите папку результатов"), m_outputDirectoryEdit->text());
        if (!directory.isEmpty())
            m_outputDirectoryEdit->setText(QDir::toNativeSeparators(directory));
    });
    connect(m_modeCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_intervalSpin->setEnabled(index == 1 && !m_sessionActive);
    });
    connect(m_keyEdit, &QLineEdit::editingFinished, this, [this] {
        m_keyEdit->setText(m_keyEdit->text().toUpper());
    });
    connect(m_startButton, &QPushButton::clicked, this, &MainWindow::startSession);
    connect(m_pauseButton, &QPushButton::clicked, this, &MainWindow::togglePause);
    connect(m_stopButton, &QPushButton::clicked, this, &MainWindow::stopSession);
}

void MainWindow::connectWorker()
{
    connect(m_worker, &FileProcessor::batchStarted, this, [this](int count, int skipped) {
        m_batchLabel->setText(tr("Новых файлов: %1; уже обработано: %2").arg(count).arg(skipped));
        m_pauseButton->setEnabled(count > 0 && !m_stopRequestedByUser);
        if (count == 0) {
            m_statusLabel->setText(m_modeCombo->currentIndex() == 1
                                       ? tr("Новых файлов нет — ожидание")
                                       : tr("Подходящие файлы не найдены"));
        } else {
            m_statusLabel->setText(tr("Обработка…"));
        }
    });

    connect(m_worker, &FileProcessor::fileStarted, this,
            [this](const QString &path, int index, int total) {
                m_statusLabel->setText(tr("Обработка файла %1 из %2").arg(index).arg(total));
                m_currentFileLabel->setText(tr("Файл: %1").arg(QDir::toNativeSeparators(path)));
                m_progressBar->setValue(0);
                m_byteProgressLabel->setText(tr("0 Б / 0 Б"));
                m_pauseButton->setEnabled(true);
                appendLog(tr("Начата обработка: %1").arg(QDir::toNativeSeparators(path)));
            });

    connect(m_worker, &FileProcessor::fileProgress, this,
            [this](const QString &, qint64 processed, qint64 total) {
                const int value = total > 0
                                      ? static_cast<int>((processed * kProgressMaximum) / total)
                                      : kProgressMaximum;
                m_progressBar->setValue(qBound(0, value, kProgressMaximum));
                m_byteProgressLabel->setText(
                    tr("%1 / %2").arg(formatBytes(processed), formatBytes(total)));
            });

    connect(m_worker, &FileProcessor::fileFinished, this,
            [this](const QString &, const QString &output, bool success) {
                if (success) {
                    appendLog(tr("Результат сохранён: %1").arg(QDir::toNativeSeparators(output)));
                } else {
                    appendLog(tr("Файл пропущен из-за ошибки."), true);
                }
            });

    connect(m_worker, &FileProcessor::pauseStateChanged, this, [this](bool paused) {
        if (paused) {
            m_pauseRequestedByUser = true;
            m_pauseButton->setEnabled(!m_stopRequestedByUser);
            m_pauseButton->setText(tr("Продолжить"));
            m_statusLabel->setText(tr("Обработка приостановлена"));
            statusBar()->showMessage(tr("Позиция в файле сохранена в открытом потоке"));
        } else if (m_sessionActive && m_cycleActive && !m_stopRequestedByUser) {
            m_pauseRequestedByUser = false;
            m_pauseButton->setText(tr("Приостановить"));
            m_statusLabel->setText(tr("Обработка продолжена"));
        }
    });

    connect(m_worker, &FileProcessor::warningOccurred, this,
            [this](const QString &message) { appendLog(tr("Предупреждение: %1").arg(message), true); });
    connect(m_worker, &FileProcessor::errorOccurred, this,
            [this](const QString &message) { appendLog(tr("Ошибка: %1").arg(message), true); });

    connect(m_worker, &FileProcessor::batchFinished, this,
            [this](int succeeded, int failed, int skipped, bool stopped) {
                if (m_shuttingDown)
                    return;

                m_totalSucceeded += succeeded;
                m_totalFailed += failed;
                m_cycleActive = false;
                m_pauseButton->setEnabled(false);
                m_pauseButton->setText(tr("Приостановить"));
                m_pauseRequestedByUser = false;

                if (succeeded > 0 || failed > 0) {
                    appendLog(tr("Цикл завершён: успешно — %1, ошибок — %2, пропущено — %3.")
                                  .arg(succeeded)
                                  .arg(failed)
                                  .arg(skipped));
                }

                if (stopped || m_stopRequestedByUser) {
                    finishSession(tr("Остановлено пользователем"));
                } else if (m_modeCombo->currentIndex() == 1) {
                    m_statusLabel->setText(tr("Ожидание новых файлов"));
                    statusBar()->showMessage(
                        tr("Следующий поиск — каждые %1 с").arg(m_intervalSpin->value()));
                } else {
                    finishSession(tr("Обработка завершена"));
                }
            });
}

void MainWindow::restoreSettings()
{
    QSettings settings;
    const QString defaultInput = QDir::currentPath();
    const QString defaultOutput = QDir(QDir::currentPath()).filePath(QStringLiteral("output"));
    m_inputDirectoryEdit->setText(settings.value(QStringLiteral("inputDirectory"), defaultInput).toString());
    m_outputDirectoryEdit->setText(settings.value(QStringLiteral("outputDirectory"), defaultOutput).toString());
    m_filterEdit->setText(settings.value(QStringLiteral("filters"), QStringLiteral("*.bin")).toString());
    m_deleteInputsCheck->setChecked(settings.value(QStringLiteral("deleteInputs"), false).toBool());
    m_collisionCombo->setCurrentIndex(settings.value(QStringLiteral("collisionAction"), 1).toInt());
    m_modeCombo->setCurrentIndex(settings.value(QStringLiteral("mode"), 0).toInt());
    m_intervalSpin->setValue(settings.value(QStringLiteral("intervalSeconds"), 10).toInt());
    m_keyEdit->setText(settings.value(QStringLiteral("xorKey"), QStringLiteral("1234567890ABCDEF"))
                           .toString()
                           .toUpper());
    m_intervalSpin->setEnabled(m_modeCombo->currentIndex() == 1);
}

void MainWindow::saveSettings() const
{
    QSettings settings;
    settings.setValue(QStringLiteral("inputDirectory"), m_inputDirectoryEdit->text());
    settings.setValue(QStringLiteral("outputDirectory"), m_outputDirectoryEdit->text());
    settings.setValue(QStringLiteral("filters"), m_filterEdit->text());
    settings.setValue(QStringLiteral("deleteInputs"), m_deleteInputsCheck->isChecked());
    settings.setValue(QStringLiteral("collisionAction"), m_collisionCombo->currentIndex());
    settings.setValue(QStringLiteral("mode"), m_modeCombo->currentIndex());
    settings.setValue(QStringLiteral("intervalSeconds"), m_intervalSpin->value());
    settings.setValue(QStringLiteral("xorKey"), m_keyEdit->text().toUpper());
}

bool MainWindow::collectSettings(ProcessingSettings *settings)
{
    const QString inputPath = normalizedAbsolutePath(m_inputDirectoryEdit->text().trimmed());
    const QString outputPath = normalizedAbsolutePath(m_outputDirectoryEdit->text().trimmed());

    if (m_inputDirectoryEdit->text().trimmed().isEmpty() || !QFileInfo(inputPath).isDir()) {
        QMessageBox::warning(this, tr("Проверьте настройки"),
                             tr("Укажите существующую папку для поиска файлов."));
        return false;
    }
    if (m_outputDirectoryEdit->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, tr("Проверьте настройки"),
                             tr("Укажите папку для сохранения результатов."));
        return false;
    }

#ifdef Q_OS_WIN
    const bool sameDirectory = inputPath.compare(outputPath, Qt::CaseInsensitive) == 0;
#else
    const bool sameDirectory = inputPath == outputPath;
#endif
    if (sameDirectory) {
        QMessageBox::warning(
            this, tr("Проверьте настройки"),
            tr("Папки поиска и результатов должны различаться. Это защищает входные файлы "
               "от перезаписи и повторной обработки."));
        return false;
    }

    if (!QDir(outputPath).exists() && !QDir().mkpath(outputPath)) {
        QMessageBox::warning(this, tr("Проверьте настройки"),
                             tr("Не удалось создать папку результатов."));
        return false;
    }

    const QStringList filters = normalizedFilters();
    if (filters.isEmpty()) {
        QMessageBox::warning(this, tr("Проверьте настройки"),
                             tr("Введите хотя бы одну маску или имя файла."));
        return false;
    }

    const QString keyText = m_keyEdit->text().trimmed();
    if (!QRegularExpression(QStringLiteral("^[0-9A-Fa-f]{16}$")).match(keyText).hasMatch()) {
        QMessageBox::warning(this, tr("Проверьте настройки"),
                             tr("Ключ XOR должен состоять ровно из 16 шестнадцатеричных символов."));
        m_keyEdit->setFocus();
        return false;
    }

    settings->inputDirectory = inputPath;
    settings->outputDirectory = outputPath;
    settings->nameFilters = filters;
    settings->deleteInputFiles = m_deleteInputsCheck->isChecked();
    settings->existingFileAction = m_collisionCombo->currentIndex() == 0
                                       ? ExistingFileAction::Overwrite
                                       : ExistingFileAction::AddCounter;
    settings->xorKey = QByteArray::fromHex(keyText.toLatin1());
    return true;
}

QStringList MainWindow::normalizedFilters() const
{
    const QStringList rawFilters = m_filterEdit->text().split(
        QRegularExpression(QStringLiteral("[;,\\s]+")), Qt::SkipEmptyParts);
    QStringList result;
    for (QString filter : rawFilters) {
        filter = filter.trimmed();
        if (filter.startsWith(QLatin1Char('.')))
            filter.prepend(QLatin1Char('*'));
        if (!filter.isEmpty() && !result.contains(filter, Qt::CaseInsensitive))
            result.append(filter);
    }
    return result;
}

void MainWindow::startSession()
{
    if (m_sessionActive)
        return;

    ProcessingSettings settings;
    if (!collectSettings(&settings))
        return;

    saveSettings();
    m_activeSettings = settings;
    m_sessionActive = true;
    m_cycleActive = false;
    m_firstCycle = true;
    m_stopRequestedByUser = false;
    m_pauseRequestedByUser = false;
    m_totalSucceeded = 0;
    m_totalFailed = 0;
    m_progressBar->setValue(0);
    m_currentFileLabel->setText(tr("Файл: —"));
    m_byteProgressLabel->setText(tr("0 Б / 0 Б"));
    setConfigurationEnabled(false);
    m_startButton->setEnabled(false);
    m_stopButton->setEnabled(true);
    m_statusLabel->setText(tr("Поиск файлов…"));
    appendLog(tr("Запуск. Маски: %1; ключ: %2")
                  .arg(settings.nameFilters.join(QStringLiteral(", ")),
                       QString::fromLatin1(settings.xorKey.toHex().toUpper())));

    if (m_modeCombo->currentIndex() == 1) {
        m_pollTimer.setInterval(m_intervalSpin->value() * 1000);
        m_pollTimer.start();
    }
    startCycle();
}

void MainWindow::startCycle()
{
    if (!m_sessionActive || m_cycleActive || !m_worker)
        return;

    const bool newSession = m_firstCycle;
    m_firstCycle = false;
    m_cycleActive = true;
    m_statusLabel->setText(tr("Поиск файлов…"));
    m_worker->prepareCycle();

    FileProcessor *worker = m_worker;
    const ProcessingSettings settings = m_activeSettings;
    QMetaObject::invokeMethod(worker, [worker, settings, newSession] {
        worker->processBatch(settings, newSession);
    }, Qt::QueuedConnection);
}

void MainWindow::stopSession()
{
    if (!m_sessionActive)
        return;

    m_stopRequestedByUser = true;
    m_pollTimer.stop();
    m_pauseButton->setEnabled(false);
    m_statusLabel->setText(tr("Остановка…"));
    statusBar()->showMessage(tr("Завершается текущая блочная операция"));

    if (m_cycleActive && m_worker) {
        m_worker->requestStop();
    } else {
        finishSession(tr("Остановлено пользователем"));
    }
}

void MainWindow::togglePause()
{
    if (!m_cycleActive || !m_worker)
        return;

    if (!m_pauseRequestedByUser) {
        m_pauseRequestedByUser = true;
        m_worker->requestPause();
        m_pauseButton->setText(tr("Продолжить"));
        m_statusLabel->setText(tr("Приостановка…"));
    } else {
        m_pauseRequestedByUser = false;
        m_worker->requestResume();
        m_pauseButton->setText(tr("Приостановить"));
        m_statusLabel->setText(tr("Продолжение…"));
    }
}

void MainWindow::finishSession(const QString &statusText)
{
    m_pollTimer.stop();
    m_sessionActive = false;
    m_cycleActive = false;
    m_stopRequestedByUser = false;
    m_pauseRequestedByUser = false;
    setConfigurationEnabled(true);
    m_startButton->setEnabled(true);
    m_pauseButton->setEnabled(false);
    m_pauseButton->setText(tr("Приостановить"));
    m_stopButton->setEnabled(false);
    m_statusLabel->setText(statusText);
    statusBar()->showMessage(
        tr("За сеанс: успешно — %1, ошибок — %2").arg(m_totalSucceeded).arg(m_totalFailed));
    appendLog(tr("%1. За сеанс: успешно — %2, ошибок — %3.")
                  .arg(statusText)
                  .arg(m_totalSucceeded)
                  .arg(m_totalFailed));
}

void MainWindow::setConfigurationEnabled(bool enabled)
{
    m_inputDirectoryEdit->setEnabled(enabled);
    m_outputDirectoryEdit->setEnabled(enabled);
    m_filterEdit->setEnabled(enabled);
    m_deleteInputsCheck->setEnabled(enabled);
    m_collisionCombo->setEnabled(enabled);
    m_modeCombo->setEnabled(enabled);
    m_intervalSpin->setEnabled(enabled && m_modeCombo->currentIndex() == 1);
    m_keyEdit->setEnabled(enabled);
    m_inputBrowseButton->setEnabled(enabled);
    m_outputBrowseButton->setEnabled(enabled);
}

void MainWindow::appendLog(const QString &message, bool isError)
{
    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    m_logEdit->appendPlainText(QStringLiteral("[%1] %2%3")
                                  .arg(timestamp, isError ? QStringLiteral("! ") : QString(), message));
}

QString MainWindow::formatBytes(qint64 bytes)
{
    static const char *units[] = {"Б", "КиБ", "МиБ", "ГиБ", "ТиБ"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    const int precision = unit == 0 ? 0 : 2;
    return QStringLiteral("%1 %2").arg(QString::number(value, 'f', precision),
                                      QString::fromUtf8(units[unit]));
}

void MainWindow::shutdownWorker()
{
    if (!m_workerThread.isRunning())
        return;

    m_shuttingDown = true;
    m_pollTimer.stop();
    if (m_worker)
        m_worker->requestStop();
    m_workerThread.quit();
    m_workerThread.wait();
    m_worker = nullptr;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    saveSettings();
    shutdownWorker();
    event->accept();
}
