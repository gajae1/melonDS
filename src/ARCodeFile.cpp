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

#include <cstdio>
#include <cstring>
#include <cctype>
#include <memory>
#include <new>
#include <stdexcept>
#include "ARCodeFile.h"
#include "ARDatabaseDAT.h"
#include "Platform.h"

namespace melonDS
{
using namespace Platform;

// TODO: import codes from other sources (usrcheat.dat, ...)
// TODO: more user-friendly error reporting


ARCodeFile::ARCodeFile(const std::string& filename)
{
    Filename = filename;

    if (!Load())
        Error = true;
}

std::vector<ARCode> ARCodeFile::GetCodes() const noexcept
{
    if (Error)
        return {};

    std::vector<ARCode> codes;

    for (auto& item : RootCat.Children)
    {
        if (std::holds_alternative<ARCodeCat>(item))
        {
            auto& cat = std::get<ARCodeCat>(item);

            for (auto& childitem : cat.Children)
            {
                auto& code = std::get<ARCode>(childitem);
                codes.push_back(code);
            }
        }
        else
        {
            auto& code = std::get<ARCode>(item);
            codes.push_back(code);
        }
    }

    return codes;
}

bool ARCodeFile::Load()
{
    Error = true;
    std::unique_ptr<FileHandle, decltype(&CloseFile)> f(OpenFile(Filename, FileMode::ReadText), CloseFile);
    if (!f)
    {
        if (FileExists(Filename)) return false;
        Error = false;
        return true;
    }

    // Keep the current tree (and its Parent pointers) until the full read and
    // parse succeed. A failed reload must not publish a partial replacement.
    ARCodeCat root {};

    bool isincat = false;
    ARCodeCat* curcat = &root;

    bool isincode = false;
    ARCode* curcode = nullptr;

    int lastentry = 0;

    char linebuf[1024];
    while (!IsEndOfFile(f.get()))
    {
        const u64 before = FilePosition(f.get());
        if (before == UINT64_MAX) return false;
        linebuf[0] = '\0';
        if (!FileReadLine(linebuf, 1024, f.get()))
        {
            if (IsEndOfFile(f.get())) break;
            return false;
        }
        // Some hosts convert a signed readLine error to bool true. Checking
        // progress also prevents a stalled read from publishing an empty tree.
        const u64 after = FilePosition(f.get());
        if (after == UINT64_MAX || after <= before) return false;

        linebuf[1023] = '\0';

        char* start = linebuf;
        while (start[0]==' ' || start[0]=='\t')
            start++;

        if (start[0]=='#' || start[0]=='\r' || start[0]=='\n' || start[0]=='\0')
            continue;

        if (!strncasecmp(start, "ROOT", 4))
        {
            isincode = false;
            isincat = true;

            curcat = &root;
            lastentry = 0;
        }
        else if (!strncasecmp(start, "CAT", 3))
        {
            char catname[128];
            int ret, retchk;
            int onlyone;
            int consumed = 0;
            if (start[3] == ' ' && (start[4] == '0' || start[4] == '1') && start[5] == ' ')
            {
                retchk = 2;
                ret = sscanf(start, "CAT %d %127[^\r\n]%n", &onlyone, catname, &consumed);
            }
            else
            {
                // backwards compatibility
                onlyone = 0;
                retchk = 1;
                ret = sscanf(start, "CAT %127[^\r\n]%n", catname, &consumed);
            }
            catname[127] = '\0';

            if (ret < retchk || !std::strchr("\r\n", start[consumed]))
            {
                Log(LogLevel::Error, "AR: malformed CAT line: %s\n", start);
                return false;
            }

            isincode = false;
            isincat = true;

            ARCodeCat cat = {
                .Parent = &root,
                .Name = catname,
                .Description = "",
                .OnlyOneCodeEnabled = onlyone!=0,
                .Children = {}
            };
            root.Children.emplace_back(cat);
            curcat = &std::get<ARCodeCat>(root.Children.back());

            lastentry = 1;
        }
        else if (!strncasecmp(start, "CODE", 4))
        {
            int enable;
            char codename[128];
            int consumed = 0;
            int ret = sscanf(start, "CODE %d %127[^\r\n]%n", &enable, codename, &consumed);
            codename[127] = '\0';

            if (ret < 2 || !std::strchr("\r\n", start[consumed]))
            {
                Log(LogLevel::Error, "AR: malformed CODE line: %s\n", start);
                return false;
            }

            if (!isincat)
            {
                Log(LogLevel::Error, "AR: encountered CODE line with no category started\n");
                return false;
            }

            isincode = true;

            ARCode code = {
                .Parent = curcat,
                .Name = codename,
                .Description = "",
                .Enabled = enable!=0,
                .Code = {}
            };
            curcat->Children.emplace_back(code);
            curcode = &std::get<ARCode>(curcat->Children.back());

            lastentry = 2;
        }
        else if (!strncasecmp(start, "DESC", 4))
        {
            char desc[256];
            int consumed = 0;
            int ret = sscanf(start, "DESC %255[^\r\n]%n", desc, &consumed);
            desc[255] = '\0';

            if (ret < 1)
                continue;
            if (!std::strchr("\r\n", start[consumed]))
            {
                Log(LogLevel::Error, "AR: description exceeds the supported length\n");
                return false;
            }

            if (lastentry == 2)
                curcode->Description = desc;
            else if (lastentry == 1)
                curcat->Description = desc;
            else
            {
                Log(LogLevel::Error, "AR: encountered DESC line not part of anything\n");
                return false;
            }
        }
        else
        {
            u32 c0, c1;
            int ret = sscanf(start, "%08X %08X", &c0, &c1);

            if (ret < 2)
            {
                Log(LogLevel::Error, "AR: malformed data line: %s\n", start);
                return false;
            }

            if (!isincode)
            {
                Log(LogLevel::Error, "AR: encountered data line with no code started\n");
                return false;
            }

            curcode->Code.push_back(c0);
            curcode->Code.push_back(c1);
        }
    }

    if (!CloseFile(f.release())) return false;

    RootCat = std::move(root);
    for (auto& item : RootCat.Children)
    {
        if (auto* cat = std::get_if<ARCodeCat>(&item))
            cat->Parent = &RootCat;
        else
            std::get<ARCode>(item).Parent = &RootCat;
    }
    // std::list moves keep category nodes stable, so their child Parent
    // pointers already refer to the final nodes.
    FinalizeList();
    Error = false;
    return true;
}

bool ARCodeFile::Serialize(std::string& output) const
{
    if (Error || !RootCat.Name.empty() || !RootCat.Description.empty() || RootCat.OnlyOneCodeEnabled)
        return false;

    const auto validText = [](const std::string& text, size_t max, bool allowEmpty)
    {
        if (text.empty()) return allowEmpty;
        // sscanf's whitespace before the scanset would discard leading space.
        return text.size() <= max && !std::isspace(static_cast<unsigned char>(text.front())) &&
               text.find_first_of("\r\n") == std::string::npos && text.find('\0') == std::string::npos;
    };

    try
    {
        std::string text;
        const auto appendCode = [&](const ARCode& code)
        {
            if (!validText(code.Name, 127, false) || !validText(code.Description, 255, true) ||
                code.Code.size() % 2 != 0)
                return false;

            text += code.Enabled ? "CODE 1 " : "CODE 0 ";
            text += code.Name;
            text += '\n';
            if (!code.Description.empty())
                text += "DESC " + code.Description + '\n';
            for (size_t i = 0; i < code.Code.size(); i += 2)
            {
                char pair[19];
                std::snprintf(pair, sizeof(pair), "%08X %08X\n", code.Code[i], code.Code[i + 1]);
                text += pair;
            }
            text += '\n';
            return true;
        };

        bool isincat = true;
        for (const auto& item : RootCat.Children)
        {
            if (const auto* cat = std::get_if<ARCodeCat>(&item))
            {
                if (!validText(cat->Name, 127, false) || !validText(cat->Description, 255, true))
                    return false;

                text += cat->OnlyOneCodeEnabled ? "CAT 1 " : "CAT 0 ";
                text += cat->Name;
                text += '\n';
                if (!cat->Description.empty())
                    text += "DESC " + cat->Description + '\n';
                text += '\n';
                isincat = true;

                bool foundEnabled = false;
                for (const auto& child : cat->Children)
                {
                    const auto* code = std::get_if<ARCode>(&child);
                    if (!code) return false; // mch has no nested categories.
                    if (cat->OnlyOneCodeEnabled && code->Enabled && foundEnabled)
                        return false; // Loading would silently disable a code.
                    foundEnabled |= code->Enabled;
                    if (!appendCode(*code)) return false;
                }
            }
            else
            {
                const auto* code = std::get_if<ARCode>(&item);
                if (!code) return false;
                if (isincat)
                {
                    text += "ROOT\n\n";
                    isincat = false;
                }
                if (!appendCode(*code)) return false;
            }
        }

        output.swap(text);
        return true;
    }
    catch (const std::bad_alloc&)
    {
        return false;
    }
    catch (const std::length_error&)
    {
        return false;
    }
}

bool ARCodeFile::Save()
{
    std::string text;
    if (!Serialize(text)) return false;

    std::unique_ptr<FileHandle, decltype(&CloseFile)> f(OpenFile(Filename, FileMode::WriteText), CloseFile);
    if (!f) return false;

    const bool written = FileWrite(text.data(), 1, text.size(), f.get()) == text.size();
    const bool flushed = written && FileFlush(f.get());
    const bool closed = CloseFile(f.release());
    return written && flushed && closed;
}

void ARCodeFile::Import(ARDatabaseEntry& dbentry, ARCodeEnableMap& enablemap, bool clear)
{
    bool hasenablemap = !enablemap.empty();

    if (clear)
        RootCat.Children.clear();

    for (auto& item : dbentry.RootCat.Children)
    {
        if (std::holds_alternative<ARCodeCat>(item))
        {
            auto& cat = std::get<ARCodeCat>(item);

            bool shouldimport = false;
            if (hasenablemap)
            {
                for (auto& childitem: cat.Children)
                {
                    auto& code = std::get<ARCode>(childitem);
                    if (enablemap[&code])
                    {
                        shouldimport = true;
                        break;
                    }
                }
            }
            else
                shouldimport = true;

            if (!shouldimport)
                continue;

            ARCodeCat newcat = {
                .Parent = &RootCat,
                .Name = cat.Name,
                .Description = cat.Description,
                .OnlyOneCodeEnabled = cat.OnlyOneCodeEnabled,
                .Children = {}
            };
            RootCat.Children.emplace_back(newcat);
            ARCodeCat* parentptr = &std::get<ARCodeCat>(RootCat.Children.back());

            for (auto& childitem : cat.Children)
            {
                auto& code = std::get<ARCode>(childitem);
                if (hasenablemap && (!enablemap[&code]))
                    continue;

                ARCode newcode = {
                    .Parent = parentptr,
                    .Name = code.Name,
                    .Description = code.Description,
                    .Enabled = code.Enabled,
                    .Code = code.Code
                };
                parentptr->Children.emplace_back(newcode);
            }
        }
        else
        {
            auto& code = std::get<ARCode>(item);
            if (hasenablemap && (!enablemap[&code]))
                continue;

            ARCode newcode = {
                .Parent = &RootCat,
                .Name = code.Name,
                .Description = code.Description,
                .Enabled = code.Enabled,
                .Code = code.Code
            };
            RootCat.Children.emplace_back(newcode);
        }
    }

    FinalizeList();
}

void ARCodeFile::FinalizeList()
{
    for (auto& item : RootCat.Children)
    {
        if (!std::holds_alternative<ARCodeCat>(item))
            continue;

        auto& cat = std::get<ARCodeCat>(item);
        if (!cat.OnlyOneCodeEnabled)
            continue;

        // for categories that only allow one code to be enabled:
        // make sure we don't have multiple ones enabled

        bool foundone = false;
        for (auto& childitem : cat.Children)
        {
            auto& code = std::get<ARCode>(childitem);
            if (!code.Enabled) continue;
            if (foundone)
                code.Enabled = false;
            else
                foundone = true;
        }
    }
}

}
