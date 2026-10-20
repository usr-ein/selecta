// SPDX-License-Identifier: GPL-3.0-or-later
// Writes a seed corpus for every slsk fuzz target from the test samples: slsk_fuzz_seeds <corpus-dir>. Each target's
// seeds go to <corpus-dir>/<target>/; existing files are kept, so the corpus grows across runs.
#include "fixtures/Samples.h"
#include "slsk/Frame.h"
#include "slsk/Zlib.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>

#include <cstdio>

using namespace Qt::StringLiterals;
using namespace selecta::slsk;

namespace {

bool writeSeed(const QString &dir, const QString &name, const QByteArray &bytes)
{
    QFile file(dir + u'/' + name);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(bytes) == bytes.size();
}

template <typename Variant>
bool writeFrames(const QString &root, const QString &target, const QString &prefix, const QList<Variant> &samples)
{
    const QString dir = root + u'/' + target;
    if (!QDir().mkpath(dir)) {
        return false;
    }
    int index = 0;
    for (const Variant &sample : samples) {
        if (!writeSeed(dir, u"%1-%2"_s.arg(prefix).arg(index++), testing::frameOf(sample))) {
            return false;
        }
    }
    return true;
}

bool writeCorpus(const QString &root)
{
    bool ok = true;
    // The server target reads frames in either direction.
    ok = ok && writeFrames(root, u"server"_s, u"to-server"_s, testing::toServerSamples());
    ok = ok && writeFrames(root, u"server"_s, u"from-server"_s, testing::fromServerSamples());
    ok = ok && writeFrames(root, u"peer_init"_s, u"sample"_s, testing::peerInitSamples());
    ok = ok && writeFrames(root, u"peer"_s, u"sample"_s, testing::peerSamples());
    ok = ok && writeFrames(root, u"distrib"_s, u"sample"_s, testing::distribSamples());
    // The zlib target takes raw streams: the compressed payloads of the peer samples, plus an empty stream.
    const QString zlibDir = root + u"/zlib"_s;
    ok = ok && QDir().mkpath(zlibDir);
    int index = 0;
    const QList<PeerMessage> peers = testing::peerSamples();
    for (const PeerMessage &sample : peers) {
        ok = ok && writeSeed(zlibDir, u"sample-%1"_s.arg(index++), zlib::deflate(testing::payloadOf(sample)));
    }
    ok = ok && writeSeed(zlibDir, u"empty"_s, zlib::deflate({}));
    if (!ok) {
        (void)std::fputs("slsk_fuzz_seeds: could not write the corpus\n", stderr);
    }
    return ok;
}

} // namespace

int main(int argc, char *argv[])
{
    if (argc != 2) {
        (void)std::fputs("usage: slsk_fuzz_seeds <corpus-dir>\n", stderr);
        return 2;
    }
    try {
        return writeCorpus(QString::fromLocal8Bit(argv[1])) ? 0 : 1;
    } catch (...) {
        (void)std::fputs("slsk_fuzz_seeds: unexpected error\n", stderr);
        return 1;
    }
}
