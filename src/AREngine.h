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

#ifndef ARENGINE_H
#define ARENGINE_H

#include <vector>
#include <stop_token>
#include <utility>
#include "ARCodeFile.h"

namespace melonDS
{
class NDS;
class AREngine
{
public:
    AREngine(melonDS::NDS& nds);

    enum class Result { Success, InvalidCode, UnsupportedCode, Interrupted };
    struct Error { std::string Name; Result Reason; };

    std::vector<ARCode> Cheats {};
    // A frontend can cancel synchronous cheat work before waiting for a queued
    // pause/reset/stop. A default token preserves unrestricted execution.
    void SetStopToken(std::stop_token token) { StopToken = std::move(token); }
    std::vector<Error> TakeErrors();
private:
    friend class ARM;
    void RunCheats();
    Result RunCheat(const ARCode& arcode);

    melonDS::NDS& NDS;
    std::stop_token StopToken;
    std::vector<Error> Errors;
};

}
#endif // ARENGINE_H
