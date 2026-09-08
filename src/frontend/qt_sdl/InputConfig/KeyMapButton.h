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

#ifndef KEYMAPBUTTON_H
#define KEYMAPBUTTON_H

#include <QPushButton>
#include <QKeyEvent>
#include <QKeySequence>
#include "Platform.h"
#include "KeyboardInput.h"

class KeyMapButton : public QPushButton
{
    Q_OBJECT

public:
    KeyMapButton(int* mapping, bool hotkey) : QPushButton()
    {
        this->mapping = mapping;
        this->isHotkey = hotkey;

        setCheckable(true);
        setText(mappingText());
        setFocusPolicy(Qt::StrongFocus); //Fixes binding keys in macOS

        connect(this, &KeyMapButton::clicked, this, &KeyMapButton::onClick);
    }

    ~KeyMapButton()
    {
    }

protected:
    void keyPressEvent(QKeyEvent* event) override
    {
        if (!isChecked()) return QPushButton::keyPressEvent(event);

        Log(melonDS::Platform::Debug, "KEY PRESSED = %08X %08X | %08X %08X %08X\n", event->key(), (int)event->modifiers(), event->nativeVirtualKey(), event->nativeModifiers(), event->nativeScanCode());

        int key = event->key();
        int mod = event->modifiers();
        bool ismod = (key == Qt::Key_Control ||
                      key == Qt::Key_Alt ||
                      key == Qt::Key_AltGr ||
                      key == Qt::Key_Shift ||
                      key == Qt::Key_Meta);

        if (!mod)
        {
            if (key == Qt::Key_Escape) { click(); return; }
            if (key == Qt::Key_Backspace) { *mapping = -1; click(); return; }
        }

        if (isHotkey)
        {
            if (ismod)
                return;
        }

        *mapping = getEventKeyVal(event, isHotkey);
        click();
    }

    void focusOutEvent(QFocusEvent* event) override
    {
        if (isChecked())
        {
            // if we lost the focus while mapping, consider it 'done'
            click();
        }

        QPushButton::focusOutEvent(event);
    }

    bool focusNextPrevChild(bool next) override { return false; }

private slots:
    void onClick()
    {
        if (isChecked())
        {
            setText("[press key]");
        }
        else
        {
            setText(mappingText());
        }
    }

private:
    QString mappingText()
    {
        int key = *mapping;

        if (key == -1) return "None";

        QString isright = (key & (1<<31)) ? "Right " : "Left ";
        key &= ~(1<<31);

    #ifndef __APPLE__
        switch (key)
        {
        case Qt::Key_Control: return isright + "Ctrl";
        case Qt::Key_Alt:     return "Alt";
        case Qt::Key_AltGr:   return "AltGr";
        case Qt::Key_Shift:   return isright + "Shift";
        case Qt::Key_Meta:    return "Meta";
        }
    #else
        switch (key)
        {
        case Qt::Key_Control: return isright + "⌘";
        case Qt::Key_Alt:     return isright + "⌥";
        case Qt::Key_Shift:   return isright + "⇧";
        case Qt::Key_Meta:    return isright + "⌃";
        }
    #endif

        QKeySequence seq(key);
        QString ret = seq.toString(QKeySequence::NativeText);

        // weak attempt at detecting garbage key names
        //if (ret.length() == 2 && ret[0].unicode() > 0xFF)
        //    return QString("[%1]").arg(key, 8, 16);

        return ret.replace("&", "&&");
    }

    int* mapping;
    bool isHotkey;
};

#endif // KEYMAPBUTTON_H
