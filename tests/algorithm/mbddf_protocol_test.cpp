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

TEST(MbddfProtocolTest, LoadsFiveSampleHelmFeedbackAndSweepDuration)
{
    const QString directory = currentCatalogDirectory();
    if (!QFileInfo(directory).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not present";
    }
    ProtocolCatalog catalog;
    QString error;
    ASSERT_TRUE(catalog.loadFromDirectory(directory, &error)) << error.toStdString();
    const MessageDefinition* start =
        catalog.findByName(QStringLiteral("helm_start_request"));
    const MessageDefinition* feedback =
        catalog.findByName(QStringLiteral("helm_feedback_response"));
    ASSERT_NE(start, nullptr);
    ASSERT_NE(feedback, nullptr);
    EXPECT_EQ(start->payloadLength, 48);
    EXPECT_EQ(feedback->payloadLength, 232);

    QVariantMap startValues{
        {QStringLiteral("waveform"), 4},
        {QStringLiteral("freq"), 0.5},
        {QStringLiteral("ampl"), 250.0},
        {QStringLiteral("offset"), -100.0},
        {QStringLiteral("start"), 0.25},
        {QStringLiteral("max_freq"), 80.0},
        {QStringLiteral("sweep_duration_s"), 12.5},
        {QStringLiteral("enable"), 15},
    };
    QByteArray startPayload;
    ASSERT_TRUE(encodePayload(*start, startValues, 0x1234,
                              &startPayload, &error)) << error.toStdString();
    QVariantMap decodedStart;
    ASSERT_TRUE(decodePayload(*start, startPayload, &decodedStart, &error))
        << error.toStdString();
    EXPECT_NEAR(decodedStart.value(QStringLiteral("sweep_duration_s")).toDouble(),
                12.5, 1e-6);
    EXPECT_NEAR(decodedStart.value(QStringLiteral("ampl")).toDouble(),
                250.0, 1e-6);

    QVariantMap feedbackValues{
        {QStringLiteral("status"), 0},
        {QStringLiteral("err_code"), 0},
        {QStringLiteral("sample_count"), 5},
        {QStringLiteral("first_timestamp_us_low"), 0x10u},
        {QStringLiteral("first_timestamp_us_high"), 0x02u},
        {QStringLiteral("sample[4].delta_us"), 4000},
        {QStringLiteral("sample[4].serial_b"), 104},
        {QStringLiteral("sample[4].fdb[3]"), -12.5},
        {QStringLiteral("sample[4].self_check"), 3},
        {QStringLiteral("sample[4].timeout"), 1},
        {QStringLiteral("sample[4].serial_a"), 94},
        {QStringLiteral("sample[4].ins[0]"), 250.0},
    };
    QByteArray feedbackPayload;
    ASSERT_TRUE(encodePayload(*feedback, feedbackValues, 0x9000,
                              &feedbackPayload, &error)) << error.toStdString();
    EXPECT_EQ(feedbackPayload.size(), 232);
    QVariantMap decodedFeedback;
    ASSERT_TRUE(decodePayload(*feedback, feedbackPayload,
                              &decodedFeedback, &error)) << error.toStdString();
    EXPECT_EQ(decodedFeedback.value(QStringLiteral("sample_count")).toUInt(), 5u);
    EXPECT_EQ(decodedFeedback.value(QStringLiteral("sample[4].serial_a")).toUInt(),
              94u);
    EXPECT_NEAR(decodedFeedback.value(QStringLiteral("sample[4].fdb[3]")).toDouble(),
                -12.5, 1e-6);
}

} // namespace
} // namespace hwtest::algorithm::mbddf
