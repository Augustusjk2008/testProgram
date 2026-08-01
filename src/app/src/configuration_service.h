#pragma once

#include <app/configuration_types.h>
#include <app/test_application_controller.h>

#include <QByteArray>
#include <QString>
#include <QVariantMap>

namespace hwtest::app {

struct ConfigurationSourceRevisions {
    QString testConfig;
    QString station;
    QString baseHal;
};

struct ConfigurationLoadSnapshot {
    QByteArray testConfigBytes;
    QVariantMap mergedHalConfig;
    ConfigurationSourceRevisions revisions;
};

struct ConfigurationBackup {
    QString documentId;
    QString revision;
    QByteArray contents;
    bool existed = false;
};

class ConfigurationService {
public:
    ConfigurationService(QString configurationDirectory = {},
                         QString baseHalConfigPath = {});

    void configure(QString configurationDirectory,
                   QString baseHalConfigPath);
    QString configurationDirectory() const;
    QString baseHalConfigPath() const;

    ActionResult catalog(ConfigurationCatalog* output) const;
    ActionResult document(const QString& documentId,
                          ConfigurationDocument* output) const;
    ActionResult saveDocument(const QString& documentId,
                              const QString& expectedRevision,
                              const QVariantMap& value,
                              ConfigurationDocument* output,
                              ConfigurationBackup* backup = nullptr) const;
    ActionResult restoreDocument(const ConfigurationBackup& backup,
                                 const QString& committedRevision) const;
    ActionResult loadSnapshot(const QString& testConfigPath,
                              ConfigurationLoadSnapshot* output) const;
    ActionResult currentSourceRevisions(
        const QString& testConfigPath,
        ConfigurationSourceRevisions* output) const;
    ActionResult isTestDocumentSelectable(const QString& documentId,
                                          bool* output) const;
    ActionResult mergedHalConfiguration(QVariantMap* output) const;

    QString testConfigPath(const QString& documentId) const;

private:
    QString m_configurationDirectory;
    QString m_baseHalConfigPath;
};

} // namespace hwtest::app
