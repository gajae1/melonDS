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

#include <QGroupBox>
#include <QLabel>
#include <QKeyEvent>
#include <QDebug>
#include <QSignalBlocker>
#include <QTimer>

#include <SDL2/SDL.h>

#include "types.h"
#include "Platform.h"

#include "InputConfigDialog.h"
#include "ui_InputConfigDialog.h"
#include "MapButton.h"


using namespace melonDS;
InputConfigDialog* InputConfigDialog::currentDlg = nullptr;

const int dskeyorder[12] = {0, 1, 10, 11, 5, 4, 6, 7, 9, 8, 2, 3};
const char* dskeylabels[12] = {"A", "B", "X", "Y", "Left", "Right", "Up", "Down", "L", "R", "Select", "Start"};

InputConfigDialog::InputConfigDialog(QWidget* parent) : QDialog(parent), ui(new Ui::InputConfigDialog)
{
    ui->setupUi(this);
    setAttribute(Qt::WA_DeleteOnClose);

    emuInstance = ((MainWindow*)parent)->getEmuInstance();

    Config::Table& instcfg = emuInstance->getLocalConfig();
    Config::Table keycfg = instcfg.GetTable("Keyboard");
    Config::Table joycfg = instcfg.GetTable("Joystick");

    for (int i = 0; i < keypad_num; i++)
    {
        const char* btn = EmuInstance::buttonNames[dskeyorder[i]];
        keypadKeyMap[i] = keycfg.GetInt(btn);
        keypadJoyMap[i] = joycfg.GetInt(btn);
    }

    int i = 0;
    for (int hotkey : hk_addons)
    {
        const char* btn = EmuInstance::hotkeyNames[hotkey];
        addonsKeyMap[i] = keycfg.GetInt(btn);
        addonsJoyMap[i] = joycfg.GetInt(btn);
        i++;
    }

    i = 0;
    for (int hotkey : hk_general)
    {
        const char* btn = EmuInstance::hotkeyNames[hotkey];
        hkGeneralKeyMap[i] = keycfg.GetInt(btn);
        hkGeneralJoyMap[i] = joycfg.GetInt(btn);
        i++;
    }

    populatePage(ui->tabAddons, hk_addons_labels, addonsKeyMap, addonsJoyMap);
    populatePage(ui->tabHotkeysGeneral, hk_general_labels, hkGeneralKeyMap, hkGeneralJoyMap);

    joystickSelection = emuInstance->getJoystickSelection();
    refreshJoysticks();
    auto* joystickTimer = new QTimer(this);
    connect(joystickTimer, &QTimer::timeout, this, &InputConfigDialog::refreshJoysticks);
    joystickTimer->start(500);

    setupKeypadPage();

    int inst = emuInstance->getInstanceID();
    if (inst > 0)
        ui->lblInstanceNum->setText(QString("Configuring mappings for instance %1").arg(inst+1));
    else
        ui->lblInstanceNum->hide();
}

InputConfigDialog::~InputConfigDialog()
{
    auto mutex = getJoyMutex();
    SDL_LockMutex(mutex.get());
    if (previewJoystick) SDL_JoystickClose(previewJoystick);
    SDL_UnlockMutex(mutex.get());
    delete ui;
}

void InputConfigDialog::setupKeypadPage()
{
    for (int i = 0; i < keypad_num; i++)
    {
        QPushButton* pushButtonKey = this->findChild<QPushButton*>(QStringLiteral("btnKey") + dskeylabels[i]);
        QPushButton* pushButtonJoy = this->findChild<QPushButton*>(QStringLiteral("btnJoy") + dskeylabels[i]);

        KeyMapButton* keyMapButtonKey = new KeyMapButton(&keypadKeyMap[i], false);
        JoyMapButton* keyMapButtonJoy = new JoyMapButton(&keypadJoyMap[i], false);

        pushButtonKey->parentWidget()->layout()->replaceWidget(pushButtonKey, keyMapButtonKey);
        pushButtonJoy->parentWidget()->layout()->replaceWidget(pushButtonJoy, keyMapButtonJoy);

        delete pushButtonKey;
        delete pushButtonJoy;

    }
}

void InputConfigDialog::populatePage(QWidget* page,
    const std::initializer_list<const char*>& labels,
    int* keymap, int* joymap)
{
    // kind of a hack
    bool ishotkey = (page != ui->tabInput);

    QHBoxLayout* main_layout = new QHBoxLayout();

    QGroupBox* group;
    QGridLayout* group_layout;

    group = new QGroupBox("Keyboard mappings:");
    main_layout->addWidget(group);
    group_layout = new QGridLayout();
    group_layout->setSpacing(1);
    int i = 0;
    for (const char* labelStr : labels)
    {
        QLabel* label = new QLabel(QString(labelStr)+":");
        KeyMapButton* btn = new KeyMapButton(&keymap[i], ishotkey);

        group_layout->addWidget(label, i, 0);
        group_layout->addWidget(btn, i, 1);
        i++;
    }
    group_layout->setRowStretch(labels.size(), 1);
    group->setLayout(group_layout);
    group->setMinimumWidth(275);

    group = new QGroupBox("Joystick mappings:");
    main_layout->addWidget(group);
    group_layout = new QGridLayout();
    group_layout->setSpacing(1);
    i = 0;
    for (const char* labelStr : labels)
    {
        QLabel* label = new QLabel(QString(labelStr)+":");
        JoyMapButton* btn = new JoyMapButton(&joymap[i], ishotkey);

        group_layout->addWidget(label, i, 0);
        group_layout->addWidget(btn, i, 1);
        i++;
    }
    group_layout->setRowStretch(labels.size(), 1);
    group->setLayout(group_layout);
    group->setMinimumWidth(275);

    page->setLayout(main_layout);
}

void InputConfigDialog::on_InputConfigDialog_accepted()
{
    Config::Table& instcfg = emuInstance->getLocalConfig();
    Config::Table keycfg = instcfg.GetTable("Keyboard");
    Config::Table joycfg = instcfg.GetTable("Joystick");

    for (int i = 0; i < keypad_num; i++)
    {
        const char* btn = EmuInstance::buttonNames[dskeyorder[i]];
        keycfg.SetInt(btn, keypadKeyMap[i]);
        joycfg.SetInt(btn, keypadJoyMap[i]);
    }

    int i = 0;
    for (int hotkey : hk_addons)
    {
        const char* btn = EmuInstance::hotkeyNames[hotkey];
        keycfg.SetInt(btn, addonsKeyMap[i]);
        joycfg.SetInt(btn, addonsJoyMap[i]);
        i++;
    }

    i = 0;
    for (int hotkey : hk_general)
    {
        const char* btn = EmuInstance::hotkeyNames[hotkey];
        keycfg.SetInt(btn, hkGeneralKeyMap[i]);
        joycfg.SetInt(btn, hkGeneralJoyMap[i]);
        i++;
    }

    // The draft holds a session identity, never a stale combo-box index. The
    // selection can remain missing/ambiguous while keyboard mappings are saved.
    emuInstance->setJoystickSelection(joystickSelection);
    emuInstance->saveJoystickConfig();
    Config::Save();

    emuInstance->inputLoadConfig();

    closeDlg();
}

void InputConfigDialog::on_InputConfigDialog_rejected()
{
    closeDlg();
}

void InputConfigDialog::on_cbxJoystick_currentIndexChanged(int id)
{
    if (id < 0) return;
    const auto instance = ui->cbxJoystick->itemData(id).toInt();
    if (instance == -2) return; // Informational missing/ambiguous row.
    // Use the snapshot that supplied the clicked row. If it just disappeared,
    // retain that choice and show its missing/ambiguous state on refresh.
    if (instance == -1) joystickSelection.Select(-1, joystickChoices);
    else
    {
        for (const auto& device : joystickChoices)
            if (device.instance == instance) joystickSelection.Select(device.index, joystickChoices);
    }
    refreshJoysticks();
}

void InputConfigDialog::refreshJoysticks()
{
    auto mutex = getJoyMutex();
    SDL_LockMutex(mutex.get());
    std::vector<JoystickDevice> devices;
    int selected;
    {
        JoystickListLock devicesLock;
        SDL_JoystickUpdate();
        devices = ListJoysticks();
        selected = joystickSelection.Resolve(devices);
        if (previewJoystick && (selected < 0 || !SDL_JoystickGetAttached(previewJoystick) ||
            SDL_JoystickInstanceID(previewJoystick) != joystickSelection.device.instance))
        {
            SDL_JoystickClose(previewJoystick);
            previewJoystick = nullptr;
        }
        if (!previewJoystick && selected >= 0) previewJoystick = SDL_JoystickOpen(selected);
        if (selected >= 0 && !previewJoystick)
        {
            selected = -1;
            joystickSelection.status = JoystickSelection::Status::Missing;
        }
    }
    SDL_UnlockMutex(mutex.get());
    joystickChoices = devices;

    using Status = JoystickSelection::Status;
    QStringList labels{tr("No controller")};
    QList<int> ids{-1};
    int selectedRow = 0;
    for (const auto& device : devices)
    {
        QString name = QString::fromStdString(device.name);
        if (name.isEmpty()) name = tr("Controller");
        // Human-facing connection order distinguishes identical display names;
        // the item's private data uses session identity, never that order.
        labels.append(tr("%1 (controller %2)").arg(name).arg(device.index + 1));
        ids.append(device.instance);
        if (device.index == selected) selectedRow = labels.size() - 1;
    }
    QString status;
    if (joystickSelection.status == Status::Missing || joystickSelection.status == Status::Ambiguous)
    {
        const bool ambiguous = joystickSelection.status == Status::Ambiguous;
        QString name = QString::fromStdString(joystickSelection.device.name);
        if (name.isEmpty()) name = tr("Selected controller");
        labels.append(ambiguous ? tr("%1 needs reselection").arg(name) : tr("%1 unavailable").arg(name));
        ids.append(-2);
        selectedRow = labels.size() - 1;
        status = ambiguous ? tr("Cannot identify the saved controller. Select it again; another controller will not be used automatically.")
                           : tr("The selected controller is unavailable. Reconnect it or choose a controller.");
    }
    else if (joystickSelection.status == Status::Disabled)
        status = devices.empty() ? tr("No controllers connected. Keyboard input is available.")
                                 : tr("Controller input is disabled for this instance.");
    else if (joystickSelection.device.serial.empty() || joystickSelection.requireSelection)
        status = tr("This controller is identified for this connection only. Select it again after reconnecting or restarting.");
    else
        status = tr("The selected controller will be remembered when it reconnects.");
    ui->lblJoystickStatus->setText(status);

    QSignalBlocker blocker(ui->cbxJoystick);
    bool changed = ui->cbxJoystick->count() != labels.size();
    for (int i = 0; !changed && i < labels.size(); ++i)
        changed = ui->cbxJoystick->itemText(i) != labels[i] || ui->cbxJoystick->itemData(i).toInt() != ids[i];
    if (changed)
    {
        ui->cbxJoystick->clear();
        for (int i = 0; i < labels.size(); ++i) ui->cbxJoystick->addItem(labels[i], ids[i]);
    }
    ui->cbxJoystick->setCurrentIndex(selectedRow);
}

SDL_Joystick* InputConfigDialog::getJoystick()
{
    return previewJoystick;
}

std::shared_ptr<SDL_mutex> InputConfigDialog::getJoyMutex()
{
    return emuInstance->getJoyMutex();
}
