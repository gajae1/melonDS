// SPDX-License-Identifier: GPL-3.0-or-later
// Test-only headless host: no files, network, camera, microphone or AAC decoder.
// Firmware and cartridge persistence requests are intentionally non-persistent.
#include "Platform.h"
#include <algorithm>
#include <cstdlib>
namespace melonDS::Platform {
void SignalStop(StopReason, void*) {}
FileHandle* OpenFile(const std::string&, FileMode) { return nullptr; }
FileHandle* OpenLocalFile(const std::string&, FileMode) { return nullptr; }
bool LocalFileExists(const std::string&) { return false; }
#ifndef MELONDS_TEST_MEMORY_FILES
bool CloseFile(FileHandle*) { std::abort(); }
bool IsEndOfFile(FileHandle*) { std::abort(); }
bool FileReadLine(char*, int, FileHandle*) { std::abort(); }
u64 FilePosition(FileHandle*) { std::abort(); }
bool FileSeek(FileHandle*, s64, FileSeekOrigin) { std::abort(); }
void FileRewind(FileHandle*) { std::abort(); }
u64 FileRead(void*, u64, u64, FileHandle*) { std::abort(); }
bool FileFlush(FileHandle*) { std::abort(); }
u64 FileWrite(const void*, u64, u64, FileHandle*) { std::abort(); }
u64 FileWriteFormatted(FileHandle*, const char*, ...) { std::abort(); }
u64 FileLength(FileHandle*) { std::abort(); }
#endif
void WriteNDSSave(const u8*, u32, u32, u32, void*) {}
void WriteGBASave(const u8*, u32, u32, u32, void*) {}
void WriteFirmware(const Firmware&, u32, u32, void*) {}
void WriteDateTime(int, int, int, int, int, int, void*) {}
void MP_Begin(void*) {}
void MP_End(void*) {}
int MP_SendPacket(u8*, int, u64, void*) { return 0; }
int MP_RecvPacket(u8*, u64*, void*) { return 0; }
int MP_SendCmd(u8*, int, u64, void*) { return 0; }
int MP_SendReply(u8*, int, u64, u16, void*) { return 0; }
int MP_SendAck(u8*, int, u64, void*) { return 0; }
int MP_RecvHostPacket(u8*, u64*, void*) { return 0; }
u16 MP_RecvReplies(u8*, u64, u16, void*) { return 0; }
int Net_SendPacket(u8*, int, void*) { return 0; }
int Net_RecvPacket(u8*, void*) { return 0; }
void Camera_Start(int, void*) {}
void Camera_Stop(int, void*) {}
void Camera_CaptureFrame(int, u32*, int, int, bool, void*) { std::abort(); }
void Mic_Start(void*) {}
void Mic_Stop(void*) {}
int Mic_ReadInput(s16*, int, void*) { return 0; }
AACDecoder* AAC_Init() { return nullptr; }
void AAC_DeInit(AACDecoder*) {}
bool AAC_Configure(AACDecoder*, int, int) { return false; }
bool AAC_DecodeFrame(AACDecoder*, const void*, int, void*, int) { return false; }
bool Addon_KeyDown(KeyType, void*) { return false; }
float Addon_MotionQuery(MotionQueryType, void*) { return 0.f; }
void Addon_RumbleStart(u32, void*) {}
void Addon_RumbleStop(void*) {}
}
