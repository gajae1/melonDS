// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef STATELOADRESULT_H
#define STATELOADRESULT_H

enum class StateLoadResult
{
    Failed, // The pre-operation state is still usable.
    Success,
    RecoveryFailed, // Core stopped; boot or reset before running again.
};

#endif
