/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#ifndef ARCHIVEUTIL_H
#define ARCHIVEUTIL_H

#include <stdio.h>

#include <string>
#include <memory>
#include <stop_token>

#include <QVector>
#include <QDir>

#include <archive.h>
#include <archive_entry.h>

#include "types.h"

namespace Archive
{

using namespace melonDS;
// "OK" followed by sorted regular-member names, or only "Err" on failure.
// Listing checks headers/skips, not the full contents or checksums of every file.
QVector<QString> ListArchive(QString path, std::stop_token stop = {});
// Positive byte count on success; -2 for a missing member after clean EOF,
// -1 for invalid input, allocation or archive errors. Both outputs are preserved
// on failure. ROM input must be nonempty and at most 0x40000000 bytes (1 GiB).
// Cancellation returns {"Cancelled"} from listing or -3 from extraction,
// preserving output arguments. A currently blocking OS call must return first.
s32 ExtractFileFromArchive(QString path, QString wantedFile, std::unique_ptr<u8[]>& filedata, u32* filesize, std::stop_token stop = {});

}

#endif // ARCHIVEUTIL_H
