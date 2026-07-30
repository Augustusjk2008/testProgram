#pragma once

#include <algorithm/board_test_executor.h>

namespace hwtest::hal {
class IHalDevice;
}

namespace hwtest::app {

struct BoardFixtureRequirement {
    bool pxi6259 = false;
    bool pxi6733 = false;
};

BoardFixtureRequirement boardFixtureRequirement(
    const QString& algorithmId,
    const QVariantMap& normalizedRunParameters);

class HalBoardTestFixture final
    : public hwtest::algorithm::mbddf::IBoardTestFixture {
public:
    void bind6259(hwtest::hal::IHalDevice* device) noexcept;
    void bind6733(hwtest::hal::IHalDevice* device) noexcept;
    void clear() noexcept;

    hwtest::hal::HalResult<QVector<hwtest::hal::DigitalSample>>
    read6259Digital(const QVector<hwtest::hal::ResourceId>& resources,
                    int timeoutMs) override;

    hwtest::hal::HalResult<hwtest::hal::SampleTaskBlock>
    capture6259Analog(const QVector<hwtest::hal::ResourceId>& resources,
                      double sampleRateHz,
                      int samplesPerChannel,
                      int timeoutMs) override;

    hwtest::hal::HalStatus write6733Analog(
        const QMap<hwtest::hal::ResourceId, double>& values,
        int timeoutMs) override;

    void settle(int milliseconds) override;

private:
    hwtest::hal::IHalDevice* m_pxi6259 = nullptr;
    hwtest::hal::IHalDevice* m_pxi6733 = nullptr;
};

} // namespace hwtest::app
