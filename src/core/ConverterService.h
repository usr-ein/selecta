// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/Error.h"
#include "convert/EncodeSettings.h"
#include "convert/MetadataModel.h"
#include "convert/TagWriter.h"

#include <QHash>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QSet>
#include <QString>
#include <QThreadPool>

#include <memory>
#include <mutex>

namespace selecta::core {

// What the converter makes (PRD CONV-1, CONV-3, CONV-4, CONV-5).
struct ConverterOptions
{
    convert::EncodeSettings target;                     // MP3 320 CBR by default
    convert::Id3Version id3 = convert::Id3Version::V23; // for MP3, AIFF and WAV
    int artworkMax = 800;                               // longest side of the embedded cover; 0: as it is
    QString outputFolder;                               // empty: next to each original

    friend bool operator==(const ConverterOptions &a, const ConverterOptions &b)
    {
        return a.target.format == b.target.format && a.target.mp3.mode == b.target.mp3.mode &&
               a.target.mp3.bitrateKbps == b.target.mp3.bitrateKbps &&
               a.target.mp3.vbrQuality == b.target.mp3.vbrQuality && a.target.sampleRate == b.target.sampleRate &&
               a.target.bitDepth == b.target.bitDepth && a.id3 == b.id3 && a.artworkMax == b.artworkMax &&
               a.outputFolder == b.outputFolder;
    }
};

// One file in the converter, as the view shows it.
struct ConverterItem
{
    enum class State {
        Reading,    // its quality, tags and cover are being read
        Ready,      // waiting for Convert
        Converting, // progress says how far
        Done,       // outputPath is the new file, verified (CONV-7)
        Failed,     // message says why
        Cancelled,
    };

    qint64 id = 0;
    QString path; // the original, never changed
    State state = State::Reading;
    QString quality;        // "FLAC 16-bit / 44.1 kHz", once read
    QString verdict;        // what converting it gives, in one line: "MP3 320 kbps CBR · about 4× smaller"
    QStringList notices;    // the quality warnings and notes (CONV-6), warnings first
    bool askFirst = false;  // a conversion without quality gain: needs "Convert anyway" (CONV-6)
    bool refused = false;   // not offered at all (lossy to a higher-rate lossless file)
    bool confirmed = false; // "Convert anyway" ticked
    double progress = 0.0;
    QString outputPath;
    QString message;
    convert::MetadataModel tags; // what the new file gets: its own tags, then what its folder and name add (CONV-5)
    QStringList added;           // fields filled from the folder and file names
    QString cover;               // where the cover comes from: "Embedded picture", "cover.jpg from the folder", empty

    bool isFinished() const { return state == State::Done || state == State::Failed || state == State::Cancelled; }
    // Whether Convert takes it now.
    bool isConvertible() const { return state == State::Ready && !refused && (!askFirst || confirmed); }
};

// The manual converter (PRD CONV-1…7) on the engine thread: any file becomes a new file in another format, with its
// tags mapped (Vorbis comments, ID3v2.3 or 2.4), missing fields filled from the folder and file names, the folder's
// cover embedded when it has none, and the quality warnings of the advisor before anything happens. Each output is
// made in Selecta's staging folder, verified (it decodes whole, its length and tags are right), then placed next to
// the original or in the chosen folder as a new file: nothing is ever overwritten, and originals are never touched.
//
// Threading: engine thread; reading and converting run on a pool of its own and come back through a mutex-guarded
// mailbox. Ownership: the destructor cancels conversions in flight and waits for them.
class ConverterService : public QObject
{
    Q_OBJECT

public:
    struct Source; // what reading a file found (ConverterService.cpp)

    struct Options
    {
        QString stagingDir;    // outputs are made here first
        QString encodedBy;     // TENC, e.g. "Selecta 0.1.0"
        int maxConcurrent = 0; // 0: half the cores, at most 4
    };

    explicit ConverterService(Options options, QObject *parent = nullptr);
    ~ConverterService() override;
    ConverterService(const ConverterService &) = delete;
    ConverterService &operator=(const ConverterService &) = delete;
    ConverterService(ConverterService &&) = delete;
    ConverterService &operator=(ConverterService &&) = delete;

    // Adds files (a file already listed and not finished is skipped); each is read on the pool.
    void add(const QStringList &paths);
    void remove(const QList<qint64> &ids);
    // Forgets the finished items.
    void clearFinished();
    void setOptions(const ConverterOptions &options);
    const ConverterOptions &options() const { return m_settings; }
    // "Convert anyway" for a conversion without quality gain (CONV-6).
    void confirm(qint64 id, bool yes);
    // Converts these items (every convertible one when empty).
    void start(const QList<qint64> &ids = {});
    // Stops every conversion: the ones running stop at the next buffer and leave nothing behind.
    void cancel();

    QList<ConverterItem> items() const;
    // Blocks until the pool is idle and its results are recorded. For tests.
    void waitForIdle();

Q_SIGNALS:
    void itemsChanged(const QList<selecta::core::ConverterItem> &items);
    // Every conversion of a Convert has ended: how many made a new file, and how many didn't.
    void finished(int done, int failed);

private Q_SLOTS:
    void drain();

private:
    struct Entry
    {
        ConverterItem item;
        std::shared_ptr<const Source> source;
        std::shared_ptr<audio::CancellationFlag> cancel;
    };
    struct Message
    {
        enum class Kind { Read, Progress, Converted };
        Kind kind = Kind::Read;
        qint64 id = 0;
        std::shared_ptr<const Source> source; // Read
        double progress = 0.0;                // Progress
        bool ok = false;                      // Converted
        QString outputPath;
        QString error;
    };

    void post(Message message);
    void advise(Entry &entry) const;
    void publish();
    void converted(const Message &message);

    Options m_options;
    ConverterOptions m_settings;
    QList<qint64> m_order;
    QHash<qint64, Entry> m_entries;
    qint64 m_nextId = 1;
    int m_runDone = 0;
    int m_runFailed = 0;
    QThreadPool m_pool;
    std::mutex m_mutex;
    QList<Message> m_mailbox;   // guarded by m_mutex
    bool m_drainPosted = false; // guarded by m_mutex
};

} // namespace selecta::core

Q_DECLARE_METATYPE(selecta::core::ConverterItem)
Q_DECLARE_METATYPE(selecta::core::ConverterOptions)
