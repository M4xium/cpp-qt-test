#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

enum class ExistingFileAction
{
    Overwrite,
    AddCounter
};

struct ProcessingSettings
{
    QString inputDirectory;
    QString outputDirectory;
    QStringList nameFilters;
    bool deleteInputFiles = false;
    ExistingFileAction existingFileAction = ExistingFileAction::Overwrite;
    QByteArray xorKey;
};
