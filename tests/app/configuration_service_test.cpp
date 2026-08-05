#include <gtest/gtest.h>

#include "configuration_service.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

namespace hwtest::app {
namespace {

bool writeJson(const QString& path, const QJsonObject& value)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return file.write(QJsonDocument(value).toJson(QJsonDocument::Indented)) >= 0;
}

QByteArray readAll(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

QVariantMap sectionById(const QVariantMap& schema, const QString& id)
{
    for (const QVariant& value : schema.value(QStringLiteral("sections")).toList()) {
        const QVariantMap section = value.toMap();
        if (section.value(QStringLiteral("id")).toString() == id) return section;
    }
    return {};
}

QVariantMap fieldByPath(const QVariantMap& schema, const QString& path)
{
    for (const QVariant& sectionValue : schema.value(QStringLiteral("sections")).toList()) {
        for (const QVariant& fieldValue :
             sectionValue.toMap().value(QStringLiteral("fields")).toList()) {
            const QVariantMap field = fieldValue.toMap();
            if (field.value(QStringLiteral("path")).toString() == path) return field;
        }
    }
    return {};
}

QVariantMap listByPath(const QVariantMap& schema, const QString& path)
{
    for (const QVariant& sectionValue : schema.value(QStringLiteral("sections")).toList()) {
        for (const QVariant& listValue :
             sectionValue.toMap().value(QStringLiteral("lists")).toList()) {
            const QVariantMap list = listValue.toMap();
            if (list.value(QStringLiteral("path")).toString() == path) return list;
        }
    }
    return {};
}

QVariantMap columnByPath(const QVariantMap& list, const QString& path)
{
    for (const QVariant& value : list.value(QStringLiteral("columns")).toList()) {
        const QVariantMap column = value.toMap();
        if (column.value(QStringLiteral("path")).toString() == path) return column;
    }
    return {};
}

bool hasOptionValue(const QVariantMap& field, const QVariant& expected)
{
    for (const QVariant& value : field.value(QStringLiteral("options")).toList()) {
        if (value.toMap().value(QStringLiteral("value")) == expected) return true;
    }
    return false;
}

TEST(ConfigurationServiceTest, CatalogUsesPersistedOrderAndDisablesUnregisteredFiles)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString firstName = QStringLiteral("first.testcfg.json");
    const QString secondName = QStringLiteral("second.testcfg.json");
    ASSERT_TRUE(QFile::copy(QString::fromUtf8(HWTEST_APP_TEST_CONFIG),
                            directory.filePath(firstName)));
    ASSERT_TRUE(QFile::copy(QString::fromUtf8(HWTEST_APP_ELEC_HEALTH_CONFIG),
                            directory.filePath(secondName)));
    ASSERT_TRUE(writeJson(
        directory.filePath(QStringLiteral("test-config-catalog.json")),
        QJsonObject{
            {QStringLiteral("schemaVersion"), QStringLiteral("1")},
            {QStringLiteral("entries"),
             QJsonArray{QJsonObject{
                 {QStringLiteral("documentId"), firstName},
                 {QStringLiteral("enabled"), true},
                 {QStringLiteral("order"), 20},
             }}},
        }));

    ConfigurationService service(directory.path(), {});
    ConfigurationCatalog catalog;
    const ActionResult result = service.catalog(&catalog);

    ASSERT_TRUE(result.ok) << result.message.toStdString();
    ASSERT_EQ(catalog.items.size(), 2);
    EXPECT_EQ(catalog.items.at(0).documentId, firstName);
    EXPECT_TRUE(catalog.items.at(0).enabled);
    EXPECT_EQ(catalog.items.at(0).order, 20);
    EXPECT_TRUE(catalog.items.at(0).valid);
    EXPECT_EQ(catalog.items.at(1).documentId, secondName);
    EXPECT_FALSE(catalog.items.at(1).enabled);
    EXPECT_TRUE(catalog.items.at(1).valid);
    EXPECT_FALSE(catalog.revision.isEmpty());
}

TEST(ConfigurationServiceTest, SaveRejectsStaleRevisionAndKeepsImmutableIdentity)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString documentId = QStringLiteral("editable.testcfg.json");
    const QString path = directory.filePath(documentId);
    ASSERT_TRUE(QFile::copy(QString::fromUtf8(HWTEST_APP_TEST_CONFIG), path));

    ConfigurationService service(directory.path(), {});
    ConfigurationDocument original;
    ASSERT_TRUE(service.document(documentId, &original).ok);
    EXPECT_EQ(original.kind, QStringLiteral("testcfg"));
    const QByteArray originalBytes = readAll(path);

    QVariantMap draft = original.value;
    QVariantMap reportFields = draft.value(QStringLiteral("reportFields")).toMap();
    reportFields.insert(QStringLiteral("title"), QStringLiteral("已保存标题"));
    draft.insert(QStringLiteral("reportFields"), reportFields);

    ConfigurationDocument saved;
    ActionResult result = service.saveDocument(
        documentId, QStringLiteral("stale-revision"), draft, &saved);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.code, QStringLiteral("config_conflict"));
    EXPECT_EQ(readAll(path), originalBytes);

    QVariantMap changedIdentity = draft;
    changedIdentity.insert(QStringLiteral("configId"), QStringLiteral("changed"));
    result = service.saveDocument(
        documentId, original.revision, changedIdentity, &saved);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.code, QStringLiteral("config_invalid"));
    EXPECT_EQ(readAll(path), originalBytes);

    result = service.saveDocument(documentId, original.revision, draft, &saved);
    ASSERT_TRUE(result.ok) << result.message.toStdString();
    EXPECT_NE(saved.revision, original.revision);
    EXPECT_EQ(saved.value.value(QStringLiteral("reportFields")).toMap()
                  .value(QStringLiteral("title")).toString(),
              QStringLiteral("已保存标题"));
}

TEST(ConfigurationServiceTest, StationDocumentProjectsHardwareLeavesWithoutChangingBaseFile)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString basePath = directory.filePath(QStringLiteral("mbddf_pc_hal.json"));
    ASSERT_TRUE(QFile::copy(QString::fromUtf8(HWTEST_APP_HAL_CONFIG), basePath));
    const QByteArray baseBytes = readAll(basePath);

    ConfigurationService service(directory.path(), basePath);
    ConfigurationDocument station;
    ASSERT_TRUE(service.document(QStringLiteral("mbddf-station"), &station).ok);
    station.value.insert(
        QStringLiteral("control"),
        QVariantMap{{QStringLiteral("resourceId"), QStringLiteral("CONTROL_SERIAL")}});
    station.value.insert(
        QStringLiteral("devices"),
        QVariantMap{{QStringLiteral("ni6259_stimulus"),
                     QVariantMap{{QStringLiteral("physicalDeviceName"),
                                  QStringLiteral("Dev7")},
                                 {QStringLiteral("serialNumber"),
                                  QStringLiteral("62591234")}}}});
    station.value.insert(
        QStringLiteral("resources"),
        QVariantMap{
            {QStringLiteral("CONTROL_SERIAL"),
             QVariantMap{{QStringLiteral("portName"), QStringLiteral("COM77")},
                         {QStringLiteral("baudRate"), 115200}}},
            {QStringLiteral("DUT_DI3_STIM"),
             QVariantMap{{QStringLiteral("portNumber"), 1},
                         {QStringLiteral("lineNumber"), 3}}},
            {QStringLiteral("HELM_PWM1_SENSE"),
             QVariantMap{{QStringLiteral("physicalIndex"), 7}}},
        });

    ConfigurationDocument saved;
    const ActionResult savedResult = service.saveDocument(
        station.documentId, station.revision, station.value, &saved);
    ASSERT_TRUE(savedResult.ok) << savedResult.message.toStdString();

    QVariantMap merged;
    const ActionResult mergedResult = service.mergedHalConfiguration(&merged);
    ASSERT_TRUE(mergedResult.ok) << mergedResult.message.toStdString();
    EXPECT_EQ(merged.value(QStringLiteral("control")).toMap()
                  .value(QStringLiteral("resourceId")).toString(),
              QStringLiteral("CONTROL_SERIAL"));

    const QVariantMap hardware = merged.value(QStringLiteral("hardware")).toMap();
    QVariantMap board;
    for (const QVariant& value : hardware.value(QStringLiteral("devices")).toList()) {
        const QVariantMap device = value.toMap();
        if (device.value(QStringLiteral("alias")).toString() ==
            QStringLiteral("ni6259_stimulus")) {
            board = device;
            break;
        }
    }
    ASSERT_FALSE(board.isEmpty());
    EXPECT_EQ(board.value(QStringLiteral("serialNumber")).toString(),
              QStringLiteral("62591234"));
    EXPECT_EQ(board.value(QStringLiteral("properties")).toMap()
                  .value(QStringLiteral("vendor")).toMap()
                  .value(QStringLiteral("ni")).toMap()
                  .value(QStringLiteral("deviceName")).toString(),
              QStringLiteral("Dev7"));

    const QVariantMap resources = hardware.value(QStringLiteral("resources")).toMap();
    EXPECT_EQ(resources.value(QStringLiteral("CONTROL_SERIAL")).toMap()
                  .value(QStringLiteral("properties")).toMap()
                  .value(QStringLiteral("portName")).toString(),
              QStringLiteral("COM77"));
    EXPECT_EQ(resources.value(QStringLiteral("DUT_DI3_STIM")).toMap()
                  .value(QStringLiteral("properties")).toMap()
                  .value(QStringLiteral("lineNumber")).toInt(),
              3);
    EXPECT_EQ(resources.value(QStringLiteral("HELM_PWM1_SENSE")).toMap()
                  .value(QStringLiteral("physicalIndex")).toInt(),
              7);
    EXPECT_EQ(readAll(basePath), baseBytes);
}

TEST(ConfigurationControllerIntegrationTest, SavesAndReloadsCurrentTestAndStationDocuments)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString testName = QStringLiteral("current.testcfg.json");
    const QString testPath = directory.filePath(testName);
    const QString halPath = directory.filePath(QStringLiteral("mbddf_pc_hal.json"));
    ASSERT_TRUE(QFile::copy(QString::fromUtf8(HWTEST_APP_TEST_CONFIG), testPath));
    ASSERT_TRUE(QFile::copy(QString::fromUtf8(HWTEST_APP_HAL_CONFIG), halPath));
    const QByteArray baseHalBytes = readAll(halPath);

    TestApplicationController controller;
    ASSERT_TRUE(controller.configureConfigurationStorage(directory.path(), halPath).ok);
    ASSERT_TRUE(controller.loadConfigurations(testPath, halPath).ok);

    ConfigurationDocument testDocument;
    ASSERT_TRUE(controller.configurationDocument(testName, &testDocument).ok);
    QVariantMap testDraft = testDocument.value;
    QVariantMap reportFields = testDraft.value(QStringLiteral("reportFields")).toMap();
    reportFields.insert(QStringLiteral("title"), QStringLiteral("重载后的标题"));
    testDraft.insert(QStringLiteral("reportFields"), reportFields);
    ASSERT_TRUE(controller.saveConfiguration(
        testName, testDocument.revision, testDraft).ok);
    EXPECT_EQ(controller.snapshot().descriptor.title,
              QStringLiteral("重载后的标题"));

    ConfigurationDocument station;
    ASSERT_TRUE(controller.configurationDocument(
        QStringLiteral("mbddf-station"), &station).ok);
    station.value.insert(
        QStringLiteral("resources"),
        QVariantMap{{QStringLiteral("CONTROL_SERIAL"),
                     QVariantMap{{QStringLiteral("portName"),
                                  QStringLiteral("COM99")},
                                 {QStringLiteral("baudRate"), 115200}}}});
    ASSERT_TRUE(controller.saveConfiguration(
        station.documentId, station.revision, station.value).ok);
    EXPECT_EQ(controller.snapshot().serialPortName, QStringLiteral("COM99"));
    EXPECT_EQ(readAll(halPath), baseHalBytes);
}

TEST(ConfigurationControllerIntegrationTest, RollsBackACommittedDraftWhenReloadFails)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString testName = QStringLiteral("di.testcfg.json");
    const QString testPath = directory.filePath(testName);
    const QString halPath = directory.filePath(QStringLiteral("mbddf_pc_hal.json"));
    ASSERT_TRUE(QFile::copy(QString::fromUtf8(HWTEST_APP_DI_CONFIG), testPath));
    ASSERT_TRUE(QFile::copy(QString::fromUtf8(HWTEST_APP_HAL_CONFIG), halPath));
    const QByteArray originalBytes = readAll(testPath);

    TestApplicationController controller;
    ASSERT_TRUE(controller.configureConfigurationStorage(directory.path(), halPath).ok);
    ASSERT_TRUE(controller.loadConfigurations(testPath, halPath).ok);
    ConfigurationDocument document;
    ASSERT_TRUE(controller.configurationDocument(testName, &document).ok);

    QVariantMap draft = document.value;
    QVariantMap execution = draft.value(QStringLiteral("executionConfig")).toMap();
    QVariantMap stimulus = execution.value(QStringLiteral("digitalStimulus")).toMap();
    QVariantList channels = stimulus.value(QStringLiteral("channels")).toList();
    ASSERT_FALSE(channels.isEmpty());
    QVariantMap firstChannel = channels.first().toMap();
    firstChannel.insert(QStringLiteral("activeLevel"), QStringLiteral("Low"));
    channels[0] = firstChannel;
    stimulus.insert(QStringLiteral("channels"), channels);
    execution.insert(QStringLiteral("digitalStimulus"), stimulus);
    draft.insert(QStringLiteral("executionConfig"), execution);

    const ActionResult saved = controller.saveConfiguration(
        testName, document.revision, draft);

    EXPECT_FALSE(saved.ok);
    EXPECT_EQ(saved.code, QStringLiteral("config_reload_failed"));
    EXPECT_EQ(readAll(testPath), originalBytes);
    EXPECT_EQ(controller.snapshot().descriptor.configId,
              document.value.value(QStringLiteral("configId")).toString());
}

} // namespace
} // namespace hwtest::app
