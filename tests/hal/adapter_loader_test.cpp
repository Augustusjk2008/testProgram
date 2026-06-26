#include "adapter_loader.h"

#include <gtest/gtest.h>

using namespace hwtest::hal;

static HalAdapterHostApiV1 hostApi()
{
    HalAdapterHostApiV1 api {};
    api.abiVersion = HAL_ADAPTER_ABI_VERSION;
    return api;
}

TEST(AdapterLoaderTest, RejectsInvalidArguments)
{
    AdapterLoader loader;
    HalAdapterApiV1 api {};

    EXPECT_FALSE(loader.load(QString{}, hostApi(), &api));
    EXPECT_FALSE(loader.errorString().isEmpty());

    EXPECT_FALSE(loader.load(QStringLiteral(""), hostApi(), &api));
    EXPECT_FALSE(loader.load(QStringLiteral("anything"), hostApi(), nullptr));
}

TEST(AdapterLoaderTest, LoadsFixtureLibraryAndStoresPath)
{
    AdapterLoader loader;
    HalAdapterApiV1 api {};
    const QString libraryPath = QString::fromLatin1(HAL_TEST_ADAPTER_FIXTURE_PATH);

    ASSERT_TRUE(loader.load(libraryPath, hostApi(), &api));
    EXPECT_TRUE(loader.isLoaded());
    EXPECT_EQ(loader.libraryPath(), libraryPath);
    EXPECT_EQ(api.abiVersion, HAL_ADAPTER_ABI_VERSION);
    EXPECT_EQ(api.structSize, static_cast<int>(sizeof(HalAdapterApiV1)));
}

TEST(AdapterLoaderTest, ReportsMissingSymbol)
{
    AdapterLoader loader;
    HalAdapterApiV1 api {};

    const QString libraryPath = QString::fromLatin1(HAL_TEST_ADAPTER_MISSING_SYMBOL_FIXTURE_PATH);

    const bool loaded = loader.load(libraryPath,
                                    hostApi(),
                                    &api);
    EXPECT_FALSE(loaded);
    EXPECT_FALSE(loader.errorString().isEmpty());
}
