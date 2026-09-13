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

#include <bit>
#include <SDL2/SDL.h>
#include <QFileDialog>
#include <QMessageBox>

#include "types.h"
#include "Platform.h"
#include "Config.h"
#include "NDS.h"
#include "DSi.h"

#include "AudioSettingsDialog.h"
#include "ui_AudioSettingsDialog.h"
#include "main.h"

using namespace melonDS;
AudioSettingsDialog* AudioSettingsDialog::currentDlg = nullptr;


AudioSettingsDialog::AudioSettingsDialog(QWidget* parent) : QDialog(parent), ui(new Ui::AudioSettingsDialog)
{
    ui->setupUi(this);
    setAttribute(Qt::WA_DeleteOnClose);

    emuInstance = ((MainWindow*)parent)->getEmuInstance();
    auto& cfg = emuInstance->getGlobalConfig();
    auto& instcfg = emuInstance->getLocalConfig();
    bool emuActive = emuInstance->emuIsActive();

    oldInterp = cfg.GetInt("Audio.Interpolation");
    oldBitDepth = cfg.GetInt("Audio.BitDepth");
    oldLowPassCutoff = cfg.GetInt("Audio.LowPassCutoff");
    oldBufferSize = cfg.GetInt("Audio.BufferSize");
    oldVolume = instcfg.GetInt("Audio.Volume");
    oldDSiSync = instcfg.GetBool("Audio.DSiVolumeSync");

    volume = oldVolume;
    dsiSync = oldDSiSync;

    ui->cbInterpolation->addItem("None");
    ui->cbInterpolation->addItem("Linear");
    ui->cbInterpolation->addItem("Cosine");
    ui->cbInterpolation->addItem("Cubic");
    ui->cbInterpolation->addItem("Gaussian (SNES)");
    ui->cbInterpolation->setCurrentIndex(oldInterp);

    ui->cbBitDepth->addItem("Automatic");
    ui->cbBitDepth->addItem("10-bit");
    ui->cbBitDepth->addItem("16-bit");
    ui->cbBitDepth->setCurrentIndex(oldBitDepth);

    for (int frames : {32, 64, 128, 256, 512, 1024})
        ui->cbBufferSize->addItem(QString("%1 frames (%2 ms at 48 kHz)")
            .arg(frames).arg(frames / 48.0, 0, 'f', 1), frames);
    ui->cbBufferSize->setCurrentIndex(ui->cbBufferSize->findData(
        static_cast<int>(std::bit_ceil(static_cast<unsigned>(cfg.GetInt("Audio.BufferSize"))))));
    ui->lblBufferStatus->setText(emuInstance->audioOutputDescription());

    ui->sbLowPassCutoff->blockSignals(true);
    ui->sbLowPassCutoff->setValue(oldLowPassCutoff > 0 ? oldLowPassCutoff : 20000);
    ui->sbLowPassCutoff->blockSignals(false);
    ui->chkLowPass->blockSignals(true);
    ui->chkLowPass->setChecked(oldLowPassCutoff > 0);
    ui->chkLowPass->blockSignals(false);
    ui->sbLowPassCutoff->setEnabled(oldLowPassCutoff > 0);

    bool state = ui->slVolume->blockSignals(true);
    ui->slVolume->setValue(oldVolume);
    ui->slVolume->blockSignals(state);

    ui->chkSyncDSiVolume->setChecked(oldDSiSync);

    // Setup volume slider accordingly
    if (emuActive && emuInstance->getNDS()->ConsoleType == 1)
    {
        on_chkSyncDSiVolume_clicked(oldDSiSync);
    }
    else
    {
        ui->chkSyncDSiVolume->setEnabled(false);
    }

    int mictype = cfg.GetInt("Mic.InputType");

    bool isext = (mictype == micInputType_External);
    ui->cbMic->setEnabled(isext);

    const int count = SDL_GetNumAudioDevices(true);
    for (int i = 0; i < count; i++)
    {
        ui->cbMic->addItem(SDL_GetAudioDeviceName(i, true));
    }

    QString micdev = cfg.GetQString("Mic.Device");
    if (micdev == "" && count > 0)
    {
        micdev = SDL_GetAudioDeviceName(0, true);
    }

    ui->cbMic->setCurrentText(micdev);

    grpMicMode = new QButtonGroup(this);
    grpMicMode->addButton(ui->rbMicNone,     micInputType_Silence);
    grpMicMode->addButton(ui->rbMicExternal, micInputType_External);
    grpMicMode->addButton(ui->rbMicNoise,    micInputType_Noise);
    grpMicMode->addButton(ui->rbMicWav,      micInputType_Wav);
#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0)
    connect(grpMicMode, SIGNAL(buttonClicked(int)), this, SLOT(onChangeMicMode(int)));
#else
    connect(grpMicMode, SIGNAL(idClicked(int)), this, SLOT(onChangeMicMode(int)));
#endif
    grpMicMode->button(mictype)->setChecked(true);

    ui->txtMicWavPath->setText(cfg.GetQString("Mic.WavPath"));

    bool iswav = (mictype == micInputType_Wav);
    ui->txtMicWavPath->setEnabled(iswav);
    ui->btnMicWavBrowse->setEnabled(iswav);

    int inst = emuInstance->getInstanceID();
    if (inst > 0)
    {
        ui->lblInstanceNum->setText(QString("Configuring settings for instance %1").arg(inst+1));
        ui->cbInterpolation->setEnabled(false);
        ui->cbBitDepth->setEnabled(false);
        ui->cbBufferSize->setEnabled(false);
        ui->btnApplyBuffer->setEnabled(false);
        ui->chkLowPass->setEnabled(false);
        ui->sbLowPassCutoff->setEnabled(false);
        for (QAbstractButton* btn : grpMicMode->buttons())
            btn->setEnabled(false);
        ui->txtMicWavPath->setEnabled(false);
        ui->btnMicWavBrowse->setEnabled(false);
        ui->cbMic->setEnabled(false);
    }
    else
        ui->lblInstanceNum->hide();
}

AudioSettingsDialog::~AudioSettingsDialog()
{
    delete ui;
}

void AudioSettingsDialog::onSyncVolumeLevel()
{
    if (dsiSync && emuInstance->getNDS()->ConsoleType == 1)
    {
        auto dsi = static_cast<DSi*>(emuInstance->getNDS());
        volume = dsi->I2C.GetBPTWL()->GetVolumeLevel();

        bool state = ui->slVolume->blockSignals(true);
        ui->slVolume->setValue(volume);
        ui->slVolume->blockSignals(state);
    }
}

void AudioSettingsDialog::onConsoleReset()
{
    on_chkSyncDSiVolume_clicked(dsiSync);
    ui->chkSyncDSiVolume->setEnabled(emuInstance->getNDS()->ConsoleType == 1);
}

void AudioSettingsDialog::on_AudioSettingsDialog_accepted()
{
    auto& cfg = emuInstance->getGlobalConfig();
    cfg.SetQString("Mic.Device", ui->cbMic->currentText());
    cfg.SetInt("Mic.InputType", grpMicMode->checkedId());
    cfg.SetQString("Mic.WavPath", ui->txtMicWavPath->text());
    // Apply is explicit so a failed device reopen cannot silently save a buffer
    // that never became active. Unapplied combo changes are applied on OK too.
    if (ui->btnApplyBuffer->isEnabled()) on_btnApplyBuffer_clicked();

    Config::SaveWithDialog(this);

    closeDlg();
}

void AudioSettingsDialog::on_AudioSettingsDialog_rejected()
{
    if (!((MainWindow*)parent())->getEmuInstance())
    {
        closeDlg();
        return;
    }

    auto& cfg = emuInstance->getGlobalConfig();
    auto& instcfg = emuInstance->getLocalConfig();
    cfg.SetInt("Audio.Interpolation", oldInterp);
    cfg.SetInt("Audio.BitDepth", oldBitDepth);
    cfg.SetInt("Audio.LowPassCutoff", oldLowPassCutoff);
    if (cfg.GetInt("Audio.BufferSize") != oldBufferSize)
    {
        QString error;
        if (emuInstance->changeAudioBuffer(oldBufferSize, error))
            cfg.SetInt("Audio.BufferSize", oldBufferSize);
        else
            QMessageBox::warning(this, tr("Audio output"),
                tr("The previous output buffer could not be restored.\n%1").arg(error));
    }
    instcfg.SetInt("Audio.Volume", oldVolume);
    instcfg.SetBool("Audio.DSiVolumeSync", oldDSiSync);

    emit updateAudioVolume(oldVolume, oldDSiSync);
    emit updateAudioSettings();

    closeDlg();
}

void AudioSettingsDialog::on_cbBitDepth_currentIndexChanged(int idx)
{
    // prevent a spurious change
    if (ui->cbBitDepth->count() < 3) return;

    auto& cfg = emuInstance->getGlobalConfig();
    cfg.SetInt("Audio.BitDepth", ui->cbBitDepth->currentIndex());

    emit updateAudioSettings();
}

void AudioSettingsDialog::on_cbInterpolation_currentIndexChanged(int idx)
{
    // prevent a spurious change
    if (ui->cbInterpolation->count() < 5) return;

    auto& cfg = emuInstance->getGlobalConfig();
    cfg.SetInt("Audio.Interpolation", ui->cbInterpolation->currentIndex());

    emit updateAudioSettings();
}

void AudioSettingsDialog::on_slVolume_valueChanged(int val)
{
    auto& cfg = emuInstance->getLocalConfig();

    if (dsiSync && emuInstance->getNDS()->ConsoleType == 1)
    {
        auto dsi = static_cast<DSi*>(emuInstance->getNDS());
        dsi->I2C.GetBPTWL()->SetVolumeLevel(val);
        return;
    }

    volume = val;
    cfg.SetInt("Audio.Volume", val);
    emit updateAudioVolume(val, dsiSync);
}

void AudioSettingsDialog::on_sbLowPassCutoff_valueChanged(int value)
{
    if (!ui->chkLowPass->isChecked()) return;
    emuInstance->getGlobalConfig().SetInt("Audio.LowPassCutoff", value);
    emit updateAudioSettings();
}

void AudioSettingsDialog::on_chkLowPass_toggled(bool checked)
{
    ui->sbLowPassCutoff->setEnabled(checked);
    emuInstance->getGlobalConfig().SetInt("Audio.LowPassCutoff",
        checked ? ui->sbLowPassCutoff->value() : 0);
    emit updateAudioSettings();
}

void AudioSettingsDialog::on_btnApplyBuffer_clicked()
{
    const int frames = ui->cbBufferSize->currentData().toInt();
    QString error;
    if (emuInstance->changeAudioBuffer(frames, error))
        emuInstance->getGlobalConfig().SetInt("Audio.BufferSize", frames);
    else
    {
        ui->cbBufferSize->setCurrentIndex(ui->cbBufferSize->findData(
            static_cast<int>(std::bit_ceil(static_cast<unsigned>(
                emuInstance->getGlobalConfig().GetInt("Audio.BufferSize"))))));
        QMessageBox::warning(this, tr("Audio output"),
            tr("The requested output buffer could not be applied.\n%1").arg(error));
    }
    ui->lblBufferStatus->setText(emuInstance->audioOutputDescription());
}

void AudioSettingsDialog::on_chkSyncDSiVolume_clicked(bool checked)
{
    dsiSync = checked;

    auto& cfg = emuInstance->getLocalConfig();
    cfg.SetBool("Audio.DSiVolumeSync", dsiSync);

    bool state = ui->slVolume->blockSignals(true);
    if (dsiSync && emuInstance->getNDS()->ConsoleType == 1)
    {
        auto dsi = static_cast<DSi*>(emuInstance->getNDS());
        ui->slVolume->setMaximum(31);
        ui->slVolume->setValue(dsi->I2C.GetBPTWL()->GetVolumeLevel());
        ui->slVolume->setPageStep(4);
        ui->slVolume->setTickPosition(QSlider::TicksBelow);
    }
    else
    {
        volume = oldVolume;
        cfg.SetInt("Audio.Volume", oldVolume);
        ui->slVolume->setMaximum(256);
        ui->slVolume->setValue(oldVolume);
        ui->slVolume->setPageStep(16);
        ui->slVolume->setTickPosition(QSlider::NoTicks);
    }
    ui->slVolume->blockSignals(state);

    emit updateAudioVolume(volume, dsiSync);
}

void AudioSettingsDialog::onChangeMicMode(int mode)
{
    bool iswav = (mode == 3);
    bool isext = (mode == 1);
    ui->txtMicWavPath->setEnabled(iswav);
    ui->btnMicWavBrowse->setEnabled(iswav);
    ui->cbMic->setEnabled(isext);
}

void AudioSettingsDialog::on_btnMicWavBrowse_clicked()
{
    QString file = QFileDialog::getOpenFileName(this,
                                                "Select WAV file...",
                                                emuDirectory,
                                                "WAV files (*.wav);;Any file (*.*)");

    if (file.isEmpty()) return;

    ui->txtMicWavPath->setText(file);
}
