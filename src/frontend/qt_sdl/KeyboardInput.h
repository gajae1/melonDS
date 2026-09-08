// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef KEYBOARDINPUT_H
#define KEYBOARDINPUT_H

#include <QKeyEvent>

bool isRightModKey(QKeyEvent* event);
// Keypad and left/right modifier identity are part of a physical binding.
// Shortcut modifiers apply only to hotkeys, not to DS buttons.
inline constexpr int KeyboardShortcutModifiers = Qt::ShiftModifier | Qt::ControlModifier |
    Qt::AltModifier | Qt::MetaModifier | Qt::GroupSwitchModifier;
int getEventKeyVal(QKeyEvent* event, bool hotkey = true);

#endif
