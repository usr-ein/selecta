// SPDX-License-Identifier: GPL-3.0-or-later
#include "convert/MetadataModel.h"

using namespace Qt::StringLiterals;

Q_LOGGING_CATEGORY(lcConvert, "selecta.convert")

namespace selecta::convert {

bool MetadataModel::contains(QStringView key) const
{
    return m_fields.contains(key.toString().toUpper());
}

QStringList MetadataModel::values(QStringView key) const
{
    return m_fields.value(key.toString().toUpper()).values;
}

QString MetadataModel::value(QStringView key) const
{
    const QStringList list = values(key);
    return list.isEmpty() ? QString() : list.first();
}

Provenance MetadataModel::provenance(QStringView key) const
{
    return m_fields.value(key.toString().toUpper()).provenance;
}

void MetadataModel::set(QStringView key, const QStringList &values, Provenance provenance)
{
    const QString name = key.toString().toUpper();
    if (name.isEmpty()) {
        return;
    }
    QStringList kept;
    for (const QString &v : values) {
        if (!v.isEmpty()) {
            kept.append(v);
        }
    }
    if (kept.isEmpty()) {
        m_fields.remove(name);
        return;
    }
    m_fields.insert(name, Field{kept, provenance});
}

void MetadataModel::set(QStringView key, const QString &value, Provenance provenance)
{
    set(key, QStringList{value}, provenance);
}

void MetadataModel::remove(QStringView key)
{
    m_fields.remove(key.toString().toUpper());
}

std::optional<Picture> MetadataModel::frontCover() const
{
    for (const Picture &picture : m_pictures) {
        if (picture.isFrontCover()) {
            return picture;
        }
    }
    return std::nullopt;
}

void MetadataModel::setFrontCover(const Picture &picture)
{
    Picture cover = picture;
    cover.type = Picture::kTypeFrontCover;
    m_pictures = {cover};
}

void MetadataModel::supplementFrom(const MetadataModel &other)
{
    for (auto it = other.m_fields.cbegin(); it != other.m_fields.cend(); ++it) {
        if (!m_fields.contains(it.key())) {
            m_fields.insert(it.key(), it.value());
        }
    }
    if (!frontCover()) {
        if (const std::optional<Picture> cover = other.frontCover()) {
            m_pictures.append(*cover);
        }
    }
}

void MetadataModel::overrideFrom(const MetadataModel &other)
{
    for (auto it = other.m_fields.cbegin(); it != other.m_fields.cend(); ++it) {
        m_fields.insert(it.key(), it.value());
    }
    if (const std::optional<Picture> cover = other.frontCover()) {
        setFrontCover(*cover);
    }
}

QString provenanceName(Provenance provenance)
{
    switch (provenance) {
    case Provenance::Unknown:
        return u"unknown"_s;
    case Provenance::Embedded:
        return u"embedded"_s;
    case Provenance::Release:
        return u"release"_s;
    case Provenance::Folder:
        return u"folder"_s;
    case Provenance::FileName:
        return u"file name"_s;
    case Provenance::User:
        return u"user"_s;
    case Provenance::Selecta:
        return u"Selecta"_s;
    }
    return u"unknown"_s;
}

} // namespace selecta::convert
