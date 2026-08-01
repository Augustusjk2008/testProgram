#pragma once

#include <QString>
#include <QVariantMap>
#include <QVector>

namespace hwtest::app {

struct ConfigurationCatalogItem {
    QString documentId;
    QString configId;
    QString title;
    QString description;
    QString algorithmId;
    bool enabled = false;
    int order = 0;
    bool valid = false;
    QString message;
};

struct ConfigurationCatalog {
    QString revision;
    QVector<ConfigurationCatalogItem> items;
};

struct ConfigurationDocument {
    QString documentId;
    QString kind;
    QString revision;
    QVariantMap value;
    QVariantMap schema;
};

} // namespace hwtest::app

