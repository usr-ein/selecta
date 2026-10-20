// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QStringList>
#include <QStringView>

// Internal: a peer's search query, Nicotine+'s way (sharing note §4.2). Pure; any thread.
namespace selecta::share::detail {

struct ParsedQuery
{
    QStringList plain;    // folded words every result must have, distinct, in query order
    QStringList partial;  // folded word endings from "*word": every result must have a word ending so
    QStringList excluded; // folded words from "-word": no result may have one

    // A query needs at least one plain word of 3 or more code points (contract): otherwise it gets no answer.
    bool isAnswerable() const;
};

// Splits `query` on whitespace into terms; a term starting with `-` excludes, one starting with `*` is a word ending,
// any other is plain. Each term gives the folded words (detail::searchWords) of what follows its marker, so
// "-live-mix" excludes both "live" and "mix", as Nicotine+ does.
ParsedQuery parseQuery(QStringView query);

} // namespace selecta::share::detail
