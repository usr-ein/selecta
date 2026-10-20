// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/CountryDatabase.h"

#include <QFile>
#include <QFileInfo>
#include <QLocale>

#include <cstring>
#include <maxminddb.h>
#include <netinet/in.h>
#include <sys/socket.h>

using namespace Qt::StringLiterals;

namespace selecta::core {

bool isCountryCode(QStringView code)
{
    if (code.size() != 2) {
        return false;
    }
    for (const QChar letter : code) {
        if (letter < u'A' || letter > u'Z') {
            return false;
        }
    }
    return QLocale::codeToTerritory(code) != QLocale::AnyTerritory;
}

CountryDatabase::CountryDatabase()
    : m_db(std::make_unique<MMDB_s>())
{}

CountryDatabase::~CountryDatabase()
{
    if (m_open) {
        MMDB_close(m_db.get());
    }
}

std::shared_ptr<const CountryDatabase> CountryDatabase::open(const QString &path, QString *error, qint64 maxBytes)
{
    const auto fail = [error](const QString &why) {
        if (error != nullptr) {
            *error = why;
        }
        return std::shared_ptr<const CountryDatabase>();
    };
    const QFileInfo info(path);
    if (!info.isFile()) {
        return fail(u"no country database at %1"_s.arg(path));
    }
    if (info.size() <= 0 || info.size() > maxBytes) {
        return fail(u"the country database is %1 bytes, outside 1…%2"_s.arg(info.size()).arg(maxBytes));
    }
    std::shared_ptr<CountryDatabase> database(new CountryDatabase());
    const QByteArray name = QFile::encodeName(path);
    if (const int status = MMDB_open(name.constData(), MMDB_MODE_MMAP, database->m_db.get()); status != MMDB_SUCCESS) {
        return fail(u"the country database can't be read: %1"_s.arg(QString::fromUtf8(MMDB_strerror(status))));
    }
    database->m_open = true;
    const char *type = database->m_db->metadata.database_type;
    if (type == nullptr || !QString::fromUtf8(type).contains(u"country"_s, Qt::CaseInsensitive)) {
        return fail(u"%1 isn't a country database"_s.arg(path));
    }
    // Sample lookups walk the tree and the data section: a file broken there fails here, not on every peer.
    for (const QString &sample : {u"8.8.8.8"_s, u"1.1.1.1"_s, u"2001:4860:4860::8888"_s}) {
        if (!database->find(QHostAddress(sample))) {
            return fail(u"the country database is damaged"_s);
        }
    }
    return database;
}

QString CountryDatabase::countryOf(const QHostAddress &address) const
{
    return find(address).value_or(QString());
}

std::optional<QString> CountryDatabase::find(const QHostAddress &address) const
{
    if (!m_open) {
        return QString();
    }
    QHostAddress host = address;
    bool isV4 = false;
    const quint32 v4 = host.toIPv4Address(&isV4); // a v4-mapped IPv6 address too
    if (isV4) {
        host = QHostAddress(v4);
    }
    if (host.isNull() || !host.isGlobal() || (!isV4 && m_db->metadata.ip_version != 6)) {
        return QString();
    }
    sockaddr_storage storage{};
    if (isV4) {
        sockaddr_in in{};
        in.sin_family = AF_INET;
        in.sin_addr.s_addr = htonl(v4);
        std::memcpy(&storage, &in, sizeof in);
    } else {
        sockaddr_in6 in6{};
        in6.sin6_family = AF_INET6;
        const Q_IPV6ADDR bytes = host.toIPv6Address();
        std::memcpy(&in6.sin6_addr, bytes.c, sizeof bytes.c);
        std::memcpy(&storage, &in6, sizeof in6);
    }
    int mmdbError = MMDB_SUCCESS;
    MMDB_lookup_result_s found =
        MMDB_lookup_sockaddr(m_db.get(), reinterpret_cast<const sockaddr *>(&storage), &mmdbError);
    if (mmdbError != MMDB_SUCCESS) {
        return std::nullopt;
    }
    if (!found.found_entry) {
        return QString();
    }
    MMDB_entry_data_s data{};
    const int status = MMDB_get_value(&found.entry, &data, "country", "iso_code", nullptr);
    if (status == MMDB_LOOKUP_PATH_DOES_NOT_MATCH_DATA_ERROR) {
        return QString(); // an entry without a country (a continent only, say)
    }
    if (status != MMDB_SUCCESS) {
        return std::nullopt;
    }
    if (!data.has_data || data.type != MMDB_DATA_TYPE_UTF8_STRING || data.data_size != 2) {
        return QString();
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access): libmaxminddb's tagged union, type checked above
    const QString code = QString::fromLatin1(data.utf8_string, 2);
    return isCountryCode(code) ? code : QString();
}

} // namespace selecta::core
