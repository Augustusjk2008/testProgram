#include <gtest/gtest.h>

#include <algorithm/mbddf_protocol.h>

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtGlobal>

namespace hwtest::algorithm::mbddf {
namespace {

QString currentCatalogDirectory()
{
    const QString configured = qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR");
    return configured.isEmpty()
        ? QStringLiteral("H:/Resources/RTLinux/Demos/MB_DDF_v2/docs/design/product_protocol_csv")
        : configured;
}

bool writeTextFile(const QString& path, const QString& text)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    return file.write(text.toUtf8()) == text.toUtf8().size();
}

QString messageCsv(const QString& typeGroup,
                   const QString& subType,
                   const QString& crcIndex = QStringLiteral("B52-53"))
{
    return QStringLiteral(
               "index,length,type,name_cn,name_en,lsb,default,is_valid\n"
               "B1,1,CONST,sync0,sync[0],,0x55,1\n"
               "B2,1,CONST,sync1,sync[1],,0xAA,1\n"
               "B3,1,U8,length,len,,48,1\n"
               "B4,1,CONST,version,version,,0x11,1\n"
               "B5,1,U8,type group,type_group,,%1,1\n"
               "B6,1,U8,sub type,sub_type,,%2,1\n"
               "B7-8,2,U16,sequence,seq,,,1\n"
               "B9-51,43,RESERVED,padding,pad,,0,1\n"
               "%3,2,U16,crc,crc,,,1\n")
        .arg(typeGroup, subType, crcIndex);
}

TEST(MbddfProtocolTest, KnownPhysicalFrameRoundTrips)
{
    const QByteArray payload("MB1", 3);
    QByteArray frame;
    QString error;

    ASSERT_TRUE(encodeFrame(payload, &frame, &error)) << error.toStdString();
    EXPECT_EQ(frame.toHex().toUpper(), QByteArray("55AA034D4231FC89"));
    EXPECT_EQ(crc16Xmodem(QByteArray::fromHex("034D4231")), 0x89FCu);

    QByteArray decoded;
    ASSERT_TRUE(decodeFrame(frame, &decoded, &error)) << error.toStdString();
    EXPECT_EQ(decoded, payload);
}

TEST(MbddfProtocolTest, RejectsCsvWithCrcOutsideFrameEnd)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(writeTextFile(directory.filePath(QStringLiteral("broken_request.csv")),
                              messageCsv(QStringLiteral("0x01"),
                                         QStringLiteral("0x01"),
                                         QStringLiteral("B51-52"))));

    ProtocolCatalog catalog;
    QString error;
    EXPECT_FALSE(catalog.loadFromDirectory(directory.path(), &error));
    EXPECT_FALSE(error.isEmpty());
}

TEST(MbddfProtocolTest, RejectsNonIntegralOrNonzeroProtocolDefaults)
{
    struct InvalidDefault {
        QString from;
        QString to;
    };
    const QList<InvalidDefault> cases = {
        {QStringLiteral("B1,1,CONST,sync0,sync[0],,0x55,1"),
         QStringLiteral("B1,1,CONST,sync0,sync[0],,85.0,1")},
        {QStringLiteral("B3,1,U8,length,len,,48,1"),
         QStringLiteral("B3,1,U8,length,len,,48.0,1")},
        {QStringLiteral("B9-51,43,RESERVED,padding,pad,,0,1"),
         QStringLiteral("B9-51,43,RESERVED,padding,pad,,0.0,1")}};

    for (const InvalidDefault& invalid : cases) {
        QTemporaryDir directory;
        ASSERT_TRUE(directory.isValid());
        QString csv = messageCsv(QStringLiteral("0x01"), QStringLiteral("0x01"));
        const QString original = csv;
        csv.replace(invalid.from, invalid.to);
        ASSERT_NE(csv, original);
        ASSERT_TRUE(writeTextFile(directory.filePath(QStringLiteral("invalid_request.csv")), csv));

        ProtocolCatalog catalog;
        QString error;
        EXPECT_FALSE(catalog.loadFromDirectory(directory.path(), &error));
        EXPECT_FALSE(error.isEmpty());
    }
}

TEST(MbddfProtocolTest, RejectsDuplicateRequestCommands)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(writeTextFile(directory.filePath(QStringLiteral("first_request.csv")),
                              messageCsv(QStringLiteral("0x01"), QStringLiteral("0x01"))));
    ASSERT_TRUE(writeTextFile(directory.filePath(QStringLiteral("second_request.csv")),
                              messageCsv(QStringLiteral("0x01"), QStringLiteral("0x01"))));

    ProtocolCatalog catalog;
    QString error;
    EXPECT_FALSE(catalog.loadFromDirectory(directory.path(), &error));
    EXPECT_FALSE(error.isEmpty());
}

TEST(MbddfProtocolTest, RejectsPayloadCodecUseOfMutatedDefinitionWhenAssetsExist)
{
    const QString directory = currentCatalogDirectory();
    if (!QFileInfo(directory).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not present";
    }

    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(directory, &error)) << error.toStdString();
    const MessageDefinition* request = catalog.findByName(QStringLiteral("system_status_request"));
    ASSERT_NE(request, nullptr);

    MessageDefinition mutated = *request;
    for (ProtocolField& field : mutated.fields) {
        if (field.nameEn == QStringLiteral("version")) {
            field.defaultValue = QVariant::fromValue<qulonglong>(0x10);
        }
    }

    QByteArray payload;
    EXPECT_FALSE(encodePayload(mutated, QVariantMap{}, 0, &payload, &error));
    EXPECT_FALSE(error.isEmpty());
}

TEST(MbddfProtocolTest, LoadsCurrentSystemStatusDefinitionsWhenAssetsExist)
{
    const QString directory = currentCatalogDirectory();
    if (!QFileInfo(directory).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not present";
    }

    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(directory, &error)) << error.toStdString();

    const MessageDefinition* request = catalog.findByName(QStringLiteral("system_status_request"));
    const MessageDefinition* response = catalog.findByName(QStringLiteral("system_status_response"));
    ASSERT_NE(request, nullptr);
    ASSERT_NE(response, nullptr);
    EXPECT_EQ(request->payloadLength, 48);
    EXPECT_EQ(response->payloadLength, 48);
    EXPECT_EQ(catalog.findByCommand(0x01, 0x01, Direction::Request), request);
    EXPECT_EQ(catalog.findByCommand(0x01, 0x01, Direction::Response), response);
    EXPECT_NE(request->findField(QStringLiteral("seq")), nullptr);
    EXPECT_NE(response->findField(QStringLiteral("status")), nullptr);
    EXPECT_NE(response->findField(QStringLiteral("err_code")), nullptr);
    EXPECT_NE(response->findField(QStringLiteral("cpu_usage")), nullptr);
}

TEST(MbddfProtocolTest, SystemStatusRoundTripWhenAssetsExist)
{
    const QString directory = currentCatalogDirectory();
    if (!QFileInfo(directory).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not present";
    }

    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(directory, &error)) << error.toStdString();

    const MessageDefinition* request = catalog.findByName(QStringLiteral("system_status_request"));
    const MessageDefinition* response = catalog.findByName(QStringLiteral("system_status_response"));
    ASSERT_NE(request, nullptr);
    ASSERT_NE(response, nullptr);

    QByteArray requestPayload;
    ASSERT_TRUE(encodePayload(*request, QVariantMap{}, 0x1234, &requestPayload, &error))
        << error.toStdString();
    QByteArray expectedRequest = QByteArray::fromHex("1101013412");
    expectedRequest.append(43, '\0');
    EXPECT_EQ(requestPayload, expectedRequest);

    QByteArray requestFrame;
    ASSERT_TRUE(encodeFrame(requestPayload, &requestFrame, &error)) << error.toStdString();
    EXPECT_EQ(requestFrame.left(3).toHex().toUpper(), QByteArray("55AA30"));
    EXPECT_EQ(requestFrame.right(2).toHex().toUpper(), QByteArray("AC1C"));

    QVariantMap input;
    input.insert(QStringLiteral("status"), 0);
    input.insert(QStringLiteral("err_code"), 0);
    input.insert(QStringLiteral("cpu_usage"), 12.5);
    input.insert(QStringLiteral("mem_usage"), 25.0);
    input.insert(QStringLiteral("rk_temp"), 42.0);
    input.insert(QStringLiteral("k7_temp"), -5.0);
    input.insert(QStringLiteral("power_on_sec"), 99u);

    QByteArray responsePayload;
    ASSERT_TRUE(encodePayload(*response, input, 0x1234, &responsePayload, &error))
        << error.toStdString();

    QVariantMap decoded;
    ASSERT_TRUE(decodePayload(*response, responsePayload, &decoded, &error))
        << error.toStdString();
    EXPECT_EQ(decoded.value(QStringLiteral("seq")).toUInt(), 0x1234u);
    EXPECT_EQ(decoded.value(QStringLiteral("status")).toUInt(), 0u);
    EXPECT_EQ(decoded.value(QStringLiteral("err_code")).toUInt(), 0u);
    EXPECT_NEAR(decoded.value(QStringLiteral("cpu_usage")).toDouble(), 12.5, 1e-6);
    EXPECT_NEAR(decoded.value(QStringLiteral("rk_temp")).toDouble(), 42.0, 1e-6);
    EXPECT_NEAR(decoded.value(QStringLiteral("k7_temp")).toDouble(), -5.0, 1e-6);
    EXPECT_EQ(decoded.value(QStringLiteral("power_on_sec")).toUInt(), 99u);
}

TEST(MbddfProtocolTest, HandlesBitFieldsScaledValuesAndReservedBytesWhenAssetsExist)
{
    const QString directory = currentCatalogDirectory();
    if (!QFileInfo(directory).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not present";
    }

    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(directory, &error)) << error.toStdString();

    const MessageDefinition* helmRequest =
        catalog.findByName(QStringLiteral("helm_board_test_request"));
    const MessageDefinition* elecHealthResponse =
        catalog.findByName(QStringLiteral("elec_health_status_response"));
    ASSERT_NE(helmRequest, nullptr);
    ASSERT_NE(elecHealthResponse, nullptr);

    QVariantMap helmValues;
    helmValues.insert(QStringLiteral("direction[0]"), 0);
    helmValues.insert(QStringLiteral("direction[1]"), 1);
    helmValues.insert(QStringLiteral("direction[2]"), 0);
    helmValues.insert(QStringLiteral("direction[3]"), 1);
    helmValues.insert(QStringLiteral("pwm_duty_percent[0]"), 10);
    helmValues.insert(QStringLiteral("pwm_duty_percent[1]"), 20);
    helmValues.insert(QStringLiteral("pwm_duty_percent[2]"), 30);
    helmValues.insert(QStringLiteral("pwm_duty_percent[3]"), 40);

    QByteArray helmPayload;
    ASSERT_TRUE(encodePayload(*helmRequest, helmValues, 3, &helmPayload, &error))
        << error.toStdString();
    ASSERT_EQ(helmPayload.size(), 48);
    EXPECT_EQ(static_cast<uchar>(helmPayload.at(5)), 0xA0u);

    QVariantMap decodedHelm;
    ASSERT_TRUE(decodePayload(*helmRequest, helmPayload, &decodedHelm, &error))
        << error.toStdString();
    EXPECT_EQ(decodedHelm.value(QStringLiteral("direction[3]")).toUInt(), 1u);
    EXPECT_EQ(decodedHelm.value(QStringLiteral("pwm_duty_percent[2]")).toUInt(), 30u);

    const ProtocolField* helmPadding = helmRequest->findField(QStringLiteral("pad"));
    ASSERT_NE(helmPadding, nullptr);
    QByteArray corruptPayload = helmPayload;
    corruptPayload[helmPadding->payloadOffset()] = static_cast<char>(1);
    EXPECT_FALSE(decodePayload(*helmRequest, corruptPayload, &decodedHelm, &error));
    EXPECT_FALSE(error.isEmpty());

    QVariantMap elecHealthValues;
    elecHealthValues.insert(QStringLiteral("status"), 0);
    elecHealthValues.insert(QStringLiteral("err_code"), 0);
    elecHealthValues.insert(QStringLiteral("value_YX"), 5.045);
    QByteArray elecHealthPayload;
    ASSERT_TRUE(encodePayload(*elecHealthResponse, elecHealthValues, 8, &elecHealthPayload, &error))
        << error.toStdString();

    QVariantMap decodedElecHealth;
    ASSERT_TRUE(decodePayload(*elecHealthResponse, elecHealthPayload, &decodedElecHealth, &error))
        << error.toStdString();
    EXPECT_NEAR(decodedElecHealth.value(QStringLiteral("value_YX")).toDouble(), 5.045, 1e-9);
}

TEST(MbddfProtocolTest, EveryCurrentDefinitionSupportsDefaultFrameRoundTripWhenAssetsExist)
{
    const QString directory = currentCatalogDirectory();
    if (!QFileInfo(directory).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not present";
    }

    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(directory, &error)) << error.toStdString();
    ASSERT_EQ(catalog.size(), 32);

    for (const MessageDefinition& definition : catalog.messages()) {
        QByteArray payload;
        ASSERT_TRUE(encodePayload(definition, QVariantMap{}, 0x0042, &payload, &error))
            << definition.name.toStdString() << ": " << error.toStdString();

        QVariantMap decoded;
        ASSERT_TRUE(decodePayload(definition, payload, &decoded, &error))
            << definition.name.toStdString() << ": " << error.toStdString();
        EXPECT_EQ(decoded.value(QStringLiteral("seq")).toUInt(), 0x0042u)
            << definition.name.toStdString();

        QByteArray frame;
        ASSERT_TRUE(encodeFrame(payload, &frame, &error))
            << definition.name.toStdString() << ": " << error.toStdString();
        QByteArray decodedPayload;
        ASSERT_TRUE(decodeFrame(frame, &decodedPayload, &error))
            << definition.name.toStdString() << ": " << error.toStdString();
        EXPECT_EQ(decodedPayload, payload) << definition.name.toStdString();
    }
}

} // namespace
} // namespace hwtest::algorithm::mbddf
