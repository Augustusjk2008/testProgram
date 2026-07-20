#include <gtest/gtest.h>

#include <QApplication>
#include <QByteArray>

int main(int argc, char* argv[])
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication application(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
