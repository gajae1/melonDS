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

#include <stdio.h>
#include <algorithm>
#include <limits>

#include "DSi.h"
#include "DSi_AES.h"
#include "DSi_NAND.h"
#include "FATIO.h"
#include "Platform.h"

#include "sha1/sha1.hpp"
#include "tiny-AES-c/aes.hpp"

#include "fatfs/ff.h"

using namespace melonDS::Platform;

namespace melonDS::DSi_NAND
{

NANDImage::NANDImage(Platform::FileHandle* nandfile, const DSiKey& es_keyY) noexcept : NANDImage(nandfile, es_keyY.data())
{
}

NANDImage::NANDImage(Platform::FileHandle* nandfile, const u8* es_keyY) noexcept
{
    if (!nandfile)
        return;

    Length = FileLength(nandfile);

    // read the nocash footer

    FileSeek(nandfile, -0x40, FileSeekOrigin::End);

    char nand_footer[16];
    const char* nand_footer_ref = "DSi eMMC CID/CPU";
    FileRead(nand_footer, 1, sizeof(nand_footer), nandfile);
    if (memcmp(nand_footer, nand_footer_ref, sizeof(nand_footer)))
    {
        // There is another copy of the footer at 000FF800h for the case
        // that by external tools the image was cut off
        // See https://problemkaputt.de/gbatek.htm#dsisdmmcimages
        FileSeek(nandfile, 0x000FF800, FileSeekOrigin::Start);
        FileRead(nand_footer, 1, sizeof(nand_footer), nandfile);
        if (memcmp(nand_footer, nand_footer_ref, sizeof(nand_footer)))
        {
            Log(LogLevel::Error, "ERROR: NAND missing nocash footer\n");
            CloseFile(nandfile);
            return;
        }
    }

    FileRead(eMMC_CID.data(), 1, sizeof(eMMC_CID), nandfile);
    FileRead(&ConsoleID, 1, sizeof(ConsoleID), nandfile);

    // init NAND crypto

    SHA1_CTX sha;
    u8 tmp[20];
    u8 keyX[16], keyY[16];

    SHA1Init(&sha);
    SHA1Update(&sha, eMMC_CID.data(), sizeof(eMMC_CID));
    SHA1Final(tmp, &sha);

    Bswap128(FATIV.data(), tmp);

    *(u32*)&keyX[0] = (u32)ConsoleID;
    *(u32*)&keyX[4] = (u32)ConsoleID ^ 0x24EE6906;
    *(u32*)&keyX[8] = (u32)(ConsoleID >> 32) ^ 0xE65B601D;
    *(u32*)&keyX[12] = (u32)(ConsoleID >> 32);

    *(u32*)&keyY[0] = 0x0AB9DC76;
    *(u32*)&keyY[4] = 0xBD4DC4D3;
    *(u32*)&keyY[8] = 0x202DDD1D;
    *(u32*)&keyY[12] = 0xE1A00005;

    DSi_AES::DeriveNormalKey(keyX, keyY, tmp);
    Bswap128(FATKey.data(), tmp);


    *(u32*)&keyX[0] = 0x4E00004A;
    *(u32*)&keyX[4] = 0x4A00004E;
    *(u32*)&keyX[8] = (u32)(ConsoleID >> 32) ^ 0xC80C4B72;
    *(u32*)&keyX[12] = (u32)ConsoleID;

    memcpy(keyY, es_keyY, sizeof(keyY));

    DSi_AES::DeriveNormalKey(keyX, keyY, tmp);
    Bswap128(ESKey.data(), tmp);

    CurFile = nandfile;
}

NANDImage::~NANDImage()
{
    if (CurFile) CloseFile(CurFile);
    CurFile = nullptr;
}

NANDImage::NANDImage(NANDImage&& other) noexcept :
    CurFile(other.CurFile),
    eMMC_CID(other.eMMC_CID),
    ConsoleID(other.ConsoleID),
    FATIV(other.FATIV),
    FATKey(other.FATKey),
    ESKey(other.ESKey),
    Length(other.Length)
{
    other.CurFile = nullptr;
}

NANDImage& NANDImage::operator=(NANDImage&& other) noexcept
{
    if (this != &other)
    {
        if (CurFile)
            CloseFile(CurFile);

        CurFile = other.CurFile;
        eMMC_CID = other.eMMC_CID;
        ConsoleID = other.ConsoleID;
        FATIV = other.FATIV;
        FATKey = other.FATKey;
        ESKey = other.ESKey;
        Length = other.Length;

        other.CurFile = nullptr;
    }

    return *this;
}

NANDMount::NANDMount(NANDImage& nand) noexcept : Image(&nand)
{
    if (!nand)
        return;

    CurFS = std::make_unique<FATFS>();
    ff_disk_open(
        [this](BYTE* buf, LBA_t sector, UINT num) {
            return this->FF_ReadNAND(buf, sector, num);
        },
        [this](const BYTE* buf, LBA_t sector, UINT num) {
            return this->FF_WriteNAND(buf, sector, num);
        },
        (LBA_t)(nand.GetLength()>>9)
    );

    FRESULT res;
    res = f_mount(CurFS.get(), "0:", 0);
    if (res != FR_OK)
    {
        Log(LogLevel::Error, "NAND mounting failed: %d\n", res);
        f_unmount("0:");
        ff_disk_close();
        return;
    }
}


NANDMount::~NANDMount() noexcept
{
    f_unmount("0:");
    ff_disk_close();
}


void NANDImage::SetupFATCrypto(AES_ctx* ctx, u32 ctr)
{
    u8 iv[16];
    memcpy(iv, FATIV.data(), sizeof(iv));

    u8 ctr_value[16] = {0};
    ctr_value[15] = ctr & 0xFF;
    ctr_value[14] = (ctr >> 8) & 0xFF;
    ctr_value[13] = (ctr >> 16) & 0xFF;
    ctr_value[12] = (ctr >> 24) & 0xFF;

    unsigned carry = 0;
    for (unsigned i = 0; i < 16; i ++) {
        unsigned j = 15-i;
        unsigned x = iv[j] + ctr_value[j] + carry;
        carry = x >= 0x100;
        iv[j] = x;
    }

    AES_init_ctx_iv(ctx, FATKey.data(), iv);
}

u32 NANDImage::ReadFATBlock(u64 addr, u32 len, u8* buf)
{
    u32 ctr = (u32)(addr >> 4);

    AES_ctx ctx;
    SetupFATCrypto(&ctx, ctr);

    if (!FileSeek(CurFile, addr, FileSeekOrigin::Start) ||
        FileRead(buf, len, 1, CurFile) != 1)
        return 0;

    for (u32 i = 0; i < len; i += 16)
    {
        u8 tmp[16];
        Bswap128(tmp, &buf[i]);
        AES_CTR_xcrypt_buffer(&ctx, tmp, sizeof(tmp));
        Bswap128(&buf[i], tmp);
    }

    return len;
}

u32 NANDImage::WriteFATBlock(u64 addr, u32 len, const u8* buf)
{
    u32 ctr = (u32)(addr >> 4);

    AES_ctx ctx;
    SetupFATCrypto(&ctx, ctr);

    if (!FileSeek(CurFile, addr, FileSeekOrigin::Start)) return 0;

    for (u32 s = 0; s < len; s += 0x200)
    {
        u8 tempbuf[0x200];

        for (u32 i = 0; i < 0x200; i += 16)
        {
            u8 tmp[16];
            Bswap128(tmp, &buf[s+i]);
            AES_CTR_xcrypt_buffer(&ctx, tmp, sizeof(tmp));
            Bswap128(&tempbuf[i], tmp);
        }

        if (FileWrite(tempbuf, sizeof(tempbuf), 1, CurFile) != 1) return 0;
    }

    return len;
}


UINT NANDMount::FF_ReadNAND(BYTE* buf, LBA_t sector, UINT num)
{
    // TODO: allow selecting other partitions?
    u64 baseaddr = 0x10EE00;

    u64 blockaddr = baseaddr + (sector * 0x200ULL);

    u32 res = Image->ReadFATBlock(blockaddr, num*0x200, buf);
    return res >> 9;
}

UINT NANDMount::FF_WriteNAND(const BYTE* buf, LBA_t sector, UINT num)
{
    // TODO: allow selecting other partitions?
    u64 baseaddr = 0x10EE00;

    u64 blockaddr = baseaddr + (sector * 0x200ULL);

    u32 res = Image->WriteFATBlock(blockaddr, num*0x200, buf);
    return res >> 9;
}


bool NANDImage::ESEncrypt(u8* data, u32 len) const
{
    AES_ctx ctx;
    u8 iv[16];
    u8 mac[16];

    iv[0] = 0x02;
    for (int i = 0; i < 12; i++) iv[1+i] = data[len+0x1C-i];
    iv[13] = 0x00;
    iv[14] = 0x00;
    iv[15] = 0x01;

    AES_init_ctx_iv(&ctx, ESKey.data(), iv);

    u32 blklen = (len + 0xF) & ~0xF;
    mac[0] = 0x3A;
    for (int i = 1; i < 13; i++) mac[i] = iv[i];
    mac[13] = (blklen >> 16) & 0xFF;
    mac[14] = (blklen >> 8) & 0xFF;
    mac[15] = blklen & 0xFF;

    AES_ECB_encrypt(&ctx, mac);

    u32 coarselen = len & ~0xF;
    for (u32 i = 0; i < coarselen; i += 16)
    {
        u8 tmp[16];

        Bswap128(tmp, &data[i]);

        for (int i = 0; i < 16; i++) mac[i] ^= tmp[i];
        AES_CTR_xcrypt_buffer(&ctx, tmp, 16);
        AES_ECB_encrypt(&ctx, mac);

        Bswap128(&data[i], tmp);
    }

    u32 remlen = len - coarselen;
    if (remlen)
    {
        u8 rem[16];

        memset(rem, 0, 16);
        for (int i = 0; i < remlen; i++)
            rem[15-i] = data[coarselen+i];

        for (int i = 0; i < 16; i++) mac[i] ^= rem[i];
        AES_CTR_xcrypt_buffer(&ctx, rem, sizeof(rem));
        AES_ECB_encrypt(&ctx, mac);

        for (int i = 0; i < remlen; i++)
            data[coarselen+i] = rem[15-i];
    }

    ctx.Iv[13] = 0x00;
    ctx.Iv[14] = 0x00;
    ctx.Iv[15] = 0x00;
    AES_CTR_xcrypt_buffer(&ctx, mac, sizeof(mac));

    Bswap128(&data[len], mac);

    u8 footer[16];

    iv[0] = 0x00;
    iv[1] = 0x00;
    iv[2] = 0x00;
    for (int i = 0; i < 12; i++) iv[3+i] = data[len+0x1C-i];
    iv[15] = 0x00;

    footer[15] = 0x3A;
    footer[2] = (len >> 16) & 0xFF;
    footer[1] = (len >> 8) & 0xFF;
    footer[0] = len & 0xFF;

    AES_ctx_set_iv(&ctx, iv);
    AES_CTR_xcrypt_buffer(&ctx, footer, sizeof(footer));

    data[len+0x10] = footer[15];
    data[len+0x1D] = footer[2];
    data[len+0x1E] = footer[1];
    data[len+0x1F] = footer[0];

    return true;
}

bool NANDImage::ESDecrypt(u8* data, u32 len) const
{
    AES_ctx ctx;
    u8 iv[16];
    u8 mac[16];

    iv[0] = 0x02;
    for (int i = 0; i < 12; i++) iv[1+i] = data[len+0x1C-i];
    iv[13] = 0x00;
    iv[14] = 0x00;
    iv[15] = 0x01;

    AES_init_ctx_iv(&ctx, ESKey.data(), iv);

    u32 blklen = (len + 0xF) & ~0xF;
    mac[0] = 0x3A;
    for (int i = 1; i < 13; i++) mac[i] = iv[i];
    mac[13] = (blklen >> 16) & 0xFF;
    mac[14] = (blklen >> 8) & 0xFF;
    mac[15] = blklen & 0xFF;

    AES_ECB_encrypt(&ctx, mac);

    u32 coarselen = len & ~0xF;
    for (u32 i = 0; i < coarselen; i += 16)
    {
        u8 tmp[16];

        Bswap128(tmp, &data[i]);

        AES_CTR_xcrypt_buffer(&ctx, tmp, sizeof(tmp));
        for (int i = 0; i < 16; i++) mac[i] ^= tmp[i];
        AES_ECB_encrypt(&ctx, mac);

        Bswap128(&data[i], tmp);
    }

    u32 remlen = len - coarselen;
    if (remlen)
    {
        u8 rem[16];

        u32 ivnum = (coarselen >> 4) + 1;
        iv[13] = (ivnum >> 16) & 0xFF;
        iv[14] = (ivnum >> 8) & 0xFF;
        iv[15] = ivnum & 0xFF;

        memset(rem, 0, 16);
        AES_ctx_set_iv(&ctx, iv);
        AES_CTR_xcrypt_buffer(&ctx, rem, 16);

        for (int i = 0; i < remlen; i++)
            rem[15-i] = data[coarselen+i];

        AES_ctx_set_iv(&ctx, iv);
        AES_CTR_xcrypt_buffer(&ctx, rem, 16);
        for (int i = 0; i < 16; i++) mac[i] ^= rem[i];
        AES_ECB_encrypt(&ctx, mac);

        for (int i = 0; i < remlen; i++)
            data[coarselen+i] = rem[15-i];
    }

    ctx.Iv[13] = 0x00;
    ctx.Iv[14] = 0x00;
    ctx.Iv[15] = 0x00;
    AES_CTR_xcrypt_buffer(&ctx, mac, 16);

    u8 footer[16];

    iv[0] = 0x00;
    iv[1] = 0x00;
    iv[2] = 0x00;
    for (int i = 0; i < 12; i++) iv[3+i] = data[len+0x1C-i];
    iv[15] = 0x00;

    Bswap128(footer, &data[len+0x10]);

    AES_ctx_set_iv(&ctx, iv);
    AES_CTR_xcrypt_buffer(&ctx, footer, sizeof(footer));

    data[len+0x10] = footer[15];
    data[len+0x1D] = footer[2];
    data[len+0x1E] = footer[1];
    data[len+0x1F] = footer[0];

    u32 footerlen = footer[0] | (footer[1] << 8) | (footer[2] << 16);
    if (footerlen != len)
    {
        Log(LogLevel::Error, "ESDecrypt: bad length %d (expected %d)\n", len, footerlen);
        return false;
    }

    for (int i = 0; i < 16; i++)
    {
        if (data[len+i] != mac[15-i])
        {
            Log(LogLevel::Warn, "ESDecrypt: bad MAC\n");
            return false;
        }
    }

    return true;
}

bool NANDMount::ReadSerialData(DSiSerialData& dataS)
{
    FF_FIL file;
    FRESULT res = f_open(&file, "0:/sys/HWINFO_S.dat", FA_OPEN_EXISTING | FA_READ);
    if (res != FR_OK)
        return false;

    DSiSerialData data;
    u32 nread = 0;
    res = f_read(&file, &data, sizeof(data), &nread);
    f_close(&file);
    if (res != FR_OK || nread != sizeof(data))
        return false;

    dataS = data;
    return true;
}

bool NANDMount::ReadHardwareInfoN(DSiHardwareInfoN& dataN)
{
    FF_FIL file;
    FRESULT res = f_open(&file, "0:/sys/HWINFO_N.dat", FA_OPEN_EXISTING | FA_READ);
    if (res != FR_OK)
        return false;

    DSiHardwareInfoN data;
    u32 nread = 0;
    res = f_read(&file, data.data(), sizeof(data), &nread);
    f_close(&file);
    if (res != FR_OK || nread != sizeof(data))
        return false;

    dataN = data;
    return true;
}

void NANDMount::ReadHardwareInfo(DSiSerialData& dataS, DSiHardwareInfoN& dataN)
{
    ReadSerialData(dataS);
    ReadHardwareInfoN(dataN);
}

bool NANDMount::ReadUserData(DSiFirmwareSystemSettings& data)
{
    FF_FIL file;
    FRESULT res;
    u32 nread;

    FF_FIL f1, f2;
    int v1, v2;

    res = f_open(&f1, "0:/shared1/TWLCFG0.dat", FA_OPEN_EXISTING | FA_READ);
    if (res != FR_OK)
        v1 = -1;
    else
    {
        u8 tmp;
        f_lseek(&f1, 0x81);
        f_read(&f1, &tmp, 1, &nread);
        v1 = tmp;
    }

    res = f_open(&f2, "0:/shared1/TWLCFG1.dat", FA_OPEN_EXISTING | FA_READ);
    if (res != FR_OK)
        v2 = -1;
    else
    {
        u8 tmp;
        f_lseek(&f2, 0x81);
        f_read(&f2, &tmp, 1, &nread);
        v2 = tmp;
    }

    if (v1 < 0 && v2 < 0) return false;

    if (v2 > v1)
    {
        file = f2;
        f_close(&f1);
    }
    else
    {
        file = f1;
        f_close(&f2);
    }

    f_lseek(&file, 0);
    f_read(&file, &data, sizeof(DSiFirmwareSystemSettings), &nread);
    f_close(&file);

    return true;
}

static bool SaveUserData(const char* filename, const DSiFirmwareSystemSettings& data)
{
    FF_FIL file;
    if (FRESULT res = f_open(&file, filename, FA_OPEN_EXISTING | FA_READ | FA_WRITE); res != FR_OK)
    {
        Log(LogLevel::Error, "NAND: editing file %s failed: %d\n", filename, res);
        return false;
    }
    // TODO: If the file couldn't be opened, try creating a new one in its place
    // (after all, we have the data for that)

    u32 bytes_written = 0;
    FRESULT res = f_write(&file, &data, sizeof(DSiFirmwareSystemSettings), &bytes_written);
    f_close(&file);

    if (res != FR_OK || bytes_written != sizeof(DSiFirmwareSystemSettings))
    {
        Log(LogLevel::Error, "NAND: editing file %s failed: %d\n", filename, res);
        return false;
    }

    return true;
}

bool NANDMount::ApplyUserData(const DSiFirmwareSystemSettings& data)
{
    bool ok0 = SaveUserData("0:/shared1/TWLCFG0.dat", data);
    bool ok1 = SaveUserData("0:/shared1/TWLCFG1.dat", data);

    return ok0 && ok1;
}


void debug_listfiles(const char* path)
{
    FF_DIR dir;
    FF_FILINFO info;
    FRESULT res;

    res = f_opendir(&dir, path);
    if (res != FR_OK) return;

    for (;;)
    {
        res = f_readdir(&dir, &info);
        if (res != FR_OK) break;
        if (!info.fname[0]) break;

        char fullname[512];
        snprintf(fullname, sizeof(fullname), "%s/%s", path, info.fname);
        Log(LogLevel::Debug, "[%c] %s\n", (info.fattrib&AM_DIR)?'D':'F', fullname);

        if (info.fattrib & AM_DIR)
        {
            debug_listfiles(fullname);
        }
    }

    f_closedir(&dir);
}

bool NANDMount::ImportFile(const char* path, const u8* data, size_t len)
{
    if (!data || !len || !path || len > std::numeric_limits<FSIZE_t>::max())
        return false;

    FF_FIL file;
    FRESULT res;

    res = f_open(&file, path, FA_CREATE_ALWAYS | FA_WRITE);
    if (res != FR_OK)
    {
        return false;
    }

    u8 buf[0x1000];
    for (size_t i = 0; i < len; i += sizeof(buf))
    { // For each block in the file...
        u32 blocklen;
        if ((i + sizeof(buf)) > len)
            blocklen = len - i;
        else
            blocklen = sizeof(buf);

        u32 nwrite;
        memcpy(buf, data + i, blocklen);
        if (f_write(&file, buf, blocklen, &nwrite) != FR_OK || nwrite != blocklen)
        {
            f_close(&file);
            return false;
        }
    }

    if (f_close(&file) != FR_OK) return false;
    Log(LogLevel::Debug, "Imported file from memory to %s\n", path);

    return true;
}

bool NANDMount::ImportFile(const char* path, const char* in)
{
    FF_FIL file;
    FRESULT res;

    Platform::FileHandle* fin = OpenLocalFile(in, FileMode::Read);
    if (!fin)
        return false;

    u32 len = FileLength(fin);

    res = f_open(&file, path, FA_CREATE_ALWAYS | FA_WRITE);
    if (res != FR_OK)
    {
        CloseFile(fin);
        return false;
    }

    u8 buf[0x1000];
    for (u32 i = 0; i < len; i += sizeof(buf))
    {
        u32 blocklen;
        if ((i + sizeof(buf)) > len)
            blocklen = len - i;
        else
            blocklen = sizeof(buf);

        u32 nwrite;
        FileRead(buf, blocklen, 1, fin);
        f_write(&file, buf, blocklen, &nwrite);
    }

    CloseFile(fin);
    f_close(&file);

    Log(LogLevel::Debug, "Imported file from %s to %s\n", in, path);

    return true;
}

bool NANDMount::ExportFile(const char* path, const char* out)
{
    FF_FIL file;
    FRESULT res;

    res = f_open(&file, path, FA_OPEN_EXISTING | FA_READ);
    if (res != FR_OK)
        return false;

    u32 len = f_size(&file);

    Platform::FileHandle* fout = OpenLocalFile(out, FileMode::Write);
    if (!fout)
    {
        f_close(&file);
        return false;
    }

    u8 buf[0x1000];
    for (u32 i = 0; i < len; i += 0x1000)
    {
        u32 blocklen;
        if ((i + 0x1000) > len)
            blocklen = len - i;
        else
            blocklen = 0x1000;

        u32 nread;
        f_read(&file, buf, blocklen, &nread);
        FileWrite(buf, blocklen, 1, fout);
    }

    CloseFile(fout);
    f_close(&file);

    Log(LogLevel::Debug, "Exported file from %s to %s\n", path, out);

    return true;
}

void NANDMount::RemoveFile(const char* path)
{
    FF_FILINFO info;
    FRESULT res = f_stat(path, &info);
    if (res != FR_OK) return;

    if (info.fattrib & AM_RDO)
        f_chmod(path, 0, AM_RDO);

    f_unlink(path);
    Log(LogLevel::Debug, "Removed file at %s\n", path);
}

void NANDMount::RemoveDir(const char* path)
{
    FF_DIR dir;
    FF_FILINFO info;
    FRESULT res;

    res = f_stat(path, &info);
    if (res != FR_OK) return;

    if (info.fattrib & AM_RDO)
        f_chmod(path, 0, AM_RDO);

    res = f_opendir(&dir, path);
    if (res != FR_OK) return;

    std::vector<std::string> dirlist;
    std::vector<std::string> filelist;

    for (;;)
    {
        res = f_readdir(&dir, &info);
        if (res != FR_OK) break;
        if (!info.fname[0]) break;

        char fullname[512];
        snprintf(fullname, sizeof(fullname), "%s/%s", path, info.fname);

        if (info.fattrib & AM_RDO)
            f_chmod(path, 0, AM_RDO);

        if (info.fattrib & AM_DIR)
            dirlist.push_back(fullname);
        else
            filelist.push_back(fullname);
    }

    f_closedir(&dir);

    for (std::vector<std::string>::iterator it = dirlist.begin(); it != dirlist.end(); it++)
    {
        const char* path = (*it).c_str();
        RemoveDir(path);
    }

    for (std::vector<std::string>::iterator it = filelist.begin(); it != filelist.end(); it++)
    {
        const char* path = (*it).c_str();
        f_unlink(path);
    }

    f_unlink(path);
    Log(LogLevel::Debug, "Removed directory at %s\n", path);
}


u32 NANDMount::GetTitleVersion(u32 category, u32 titleid)
{
    FRESULT res;
    char path[256];
    snprintf(path, sizeof(path), "0:/title/%08x/%08x/content/title.tmd", category, titleid);
    FF_FIL file;
    res = f_open(&file, path, FA_OPEN_EXISTING | FA_READ);
    if (res != FR_OK)
        return 0xFFFFFFFF;

    u32 version;
    u32 nread;
    f_lseek(&file, 0x1E4);
    f_read(&file, &version, 4, &nread);
    version = (version >> 24) | ((version & 0xFF0000) >> 8) | ((version & 0xFF00) << 8) | (version << 24);

    f_close(&file);
    return version;
}

void NANDMount::ListTitles(u32 category, std::vector<u32>& titlelist)
{
    FRESULT res;
    FF_DIR titledir;
    char path[256];

    snprintf(path, sizeof(path), "0:/title/%08x", category);
    res = f_opendir(&titledir, path);
    if (res != FR_OK)
    {
        Log(LogLevel::Warn, "NAND: !! no title dir (%s)\n", path);
        return;
    }

    for (;;)
    {
        FF_FILINFO info;
        f_readdir(&titledir, &info);
        if (!info.fname[0])
            break;

        if (strlen(info.fname) != 8)
            continue;

        u32 titleid;
        if (sscanf(info.fname, "%08x", &titleid) < 1)
            continue;

        u32 version = GetTitleVersion(category, titleid);
        if (version == 0xFFFFFFFF)
            continue;

        snprintf(path, sizeof(path), "0:/title/%08x/%08x/content/%08x.app", category, titleid, version);
        FF_FILINFO appinfo;
        res = f_stat(path, &appinfo);
        if (res != FR_OK)
            continue;
        if (appinfo.fattrib & AM_DIR)
            continue;
        if (appinfo.fsize < 0x4000)
            continue;

        // title is good, add it to the list
        titlelist.push_back(titleid);
    }

    f_closedir(&titledir);
}

bool NANDMount::TitleExists(u32 category, u32 titleid)
{
    char path[256];
    snprintf(path, sizeof(path), "0:/title/%08x/%08x/content/title.tmd", category, titleid);

    FRESULT res = f_stat(path, nullptr);
    return (res == FR_OK);
}

void NANDMount::GetTitleInfo(u32 category, u32 titleid, u32& version, NDSHeader* header, NDSBanner* banner)
{
    version = GetTitleVersion(category, titleid);
    if (version == 0xFFFFFFFF)
        return;

    FRESULT res;

    char path[256];
    snprintf(path, sizeof(path), "0:/title/%08x/%08x/content/%08x.app", category, titleid, version);
    FF_FIL file;
    res = f_open(&file, path, FA_OPEN_EXISTING | FA_READ);
    if (res != FR_OK)
        return;

    u32 nread;
    f_read(&file, header, sizeof(NDSHeader), &nread);

    if (banner)
    {
        u32 banneraddr = header->BannerOffset;
        if (!banneraddr)
        {
            memset(banner, 0, sizeof(NDSBanner));
        }
        else
        {
            f_lseek(&file, banneraddr);
            f_read(&file, banner, sizeof(NDSBanner), &nread);
        }
    }

    f_close(&file);
}


bool NANDMount::CreateTicket(const char* path, u32 titleid0, u32 titleid1, u8 version)
{
    FF_FIL file;
    FRESULT res;
    u32 nwrite;

    res = f_open(&file, path, FA_CREATE_ALWAYS | FA_WRITE);
    if (res != FR_OK)
    {
        Log(LogLevel::Error, "CreateTicket: failed to create file (%d)\n", res);
        return false;
    }

    u8 ticket[0x2C4];
    memset(ticket, 0, 0x2C4);

    // signature, atleast make it look like we tried :P
    *(u32*)&ticket[0x000] = 0x01000100;
    strcpy((char*)&ticket[0x140], "Root-CA00000001-XS00000006");

    *(u32*)&ticket[0x1DC] = titleid0;
    *(u32*)&ticket[0x1E0] = titleid1;
    ticket[0x1E6] = version;

    memset(&ticket[0x222], 0xFF, 0x20);

    bool ok = Image->ESEncrypt(ticket, 0x2A4);

    ok = ok && f_write(&file, ticket, sizeof(ticket), &nwrite) == FR_OK && nwrite == sizeof(ticket);

    return f_close(&file) == FR_OK && ok;
}

bool NANDMount::CreateSaveFile(const char* path, u32 len)
{
    if (len == 0) return true;
    if (len < 0x200) return false;
    if (len > 0x8000000) return false;

    u32 clustersize, maxfiles, totsec16 = 1, fatsz16;

    // CHECKME!
    // code inspired from https://github.com/Epicpkmn11/NTM/blob/master/arm9/src/sav.c
    const u16 sectorsize = 0x200;

    // fit maximum sectors for the size
    const u32 maxsectors = std::min<u32>(len / sectorsize, 0xFFFF);
    u32 tracksize = 1;
    u32 headcount = 1;
    u32 totsec16next = 0;
    while (totsec16next <= maxsectors)
    {
        totsec16next = tracksize * (headcount + 1) * (headcount + 1);
        if (totsec16next <= maxsectors)
        {
            headcount++;
            totsec16 = totsec16next;

            tracksize++;
            totsec16next = tracksize * headcount * headcount;
            if (totsec16next <= maxsectors)
            {
                totsec16 = totsec16next;
            }
        }
    }
    totsec16next = (tracksize + 1) * headcount * headcount;
    if (totsec16next <= maxsectors)
    {
        tracksize++;
        totsec16 = totsec16next;
    }

    maxfiles = len < 0x8C000 ? 0x20 : 0x200;
    clustersize = (totsec16 > (8 << 10)) ? 8 : (totsec16 > (1 << 10) ? 4 : 1);

    #define ALIGN(v, a) (((v) % (a)) ? ((v) + (a) - ((v) % (a))) : (v))
    u16 totalclusters = ALIGN(totsec16, clustersize) / clustersize;
    u32 fatbytes = (ALIGN(totalclusters, 2) / 2) * 3; // 2 sectors -> 3 byte
    fatsz16 = ALIGN(fatbytes, sectorsize) / sectorsize;

    FF_FIL file;
    FRESULT res;
    u32 nwrite;

    res = f_open(&file, path, FA_CREATE_ALWAYS | FA_WRITE);
    if (res != FR_OK)
    {
        Log(LogLevel::Error, "CreateSaveFile: failed to create file (%d)\n", res);
        return false;
    }

    // Only the boot sector is nonzero; do not allocate the full save size.
    u8 data[0x200] = {};

    // create FAT header
    data[0x000] = 0xE9;
    memcpy(&data[0x003], "MSWIN4.1", 8);
    *(u16*)&data[0x00B] = sectorsize; // bytes per sector
    data[0x00D] = clustersize;
    *(u16*)&data[0x00E] = 1; // reserved sectors
    data[0x010] = 2; // num FATs
    *(u16*)&data[0x011] = maxfiles;
    *(u16*)&data[0x013] = totsec16;
    data[0x015] = 0xF8;
    *(u16*)&data[0x016] = fatsz16;
    *(u16*)&data[0x018] = tracksize;
    *(u16*)&data[0x01A] = headcount;
    data[0x024] = 0x05;
    data[0x026] = 0x29;
    *(u32*)&data[0x027] = 0x12345678;
    memcpy(&data[0x02B], "VOLUMELABEL", 11);
    memcpy(&data[0x036], "FAT12   ", 8);
    *(u16*)&data[0x1FE] = 0xAA55;

    bool ok = f_write(&file, data, sizeof(data), &nwrite) == FR_OK && nwrite == sizeof(data);
    u8 zeros[0x1000] = {};
    for (u32 offset = sizeof(data); ok && offset < len;)
    {
        u32 count = std::min<u32>(len - offset, sizeof(zeros));
        ok = f_write(&file, zeros, count, &nwrite) == FR_OK && nwrite == count;
        offset += count;
    }
    return f_close(&file) == FR_OK && ok;
}

// Only remove transaction-owned paths. Stop on the first error, leaving the
// remainder available for inspection/recovery. Never use the unchecked UI delete
// helpers on a backup or on the previous installation.
static bool RemoveTitleStage(const std::string& path)
{
    FF_FILINFO info;
    FRESULT res = f_stat(path.c_str(), &info);
    if (res == FR_NO_FILE || res == FR_NO_PATH) return true;
    if (res != FR_OK) return false;
    if (info.fattrib & AM_DIR)
    {
        FF_DIR dir;
        if (f_opendir(&dir, path.c_str()) != FR_OK) return false;
        std::vector<std::string> children;
        while ((res = f_readdir(&dir, &info)) == FR_OK && info.fname[0])
            children.push_back(path + "/" + info.fname);
        bool closed = f_closedir(&dir) == FR_OK;
        if (res != FR_OK || !closed) return false;
        for (const auto& child : children)
            if (!RemoveTitleStage(child)) return false;
    }
    if (f_chmod(path.c_str(), 0, AM_RDO) != FR_OK) return false;
    return f_unlink(path.c_str()) == FR_OK;
}

static bool TitleDirectory(const std::string& path)
{
    FRESULT res = f_mkdir(path.c_str());
    if (res == FR_OK) return true;
    FF_FILINFO info;
    return res == FR_EXIST && f_stat(path.c_str(), &info) == FR_OK && (info.fattrib & AM_DIR);
}

// FatFs rename is NOT power-loss atomic. In particular FR_DISK_ERR may leave
// cross-linked directory entries. Do not unlink either name in that situation.
// A non-I/O failure can be rolled back only if the names establish what moved.
static bool MoveTitlePath(const std::string& from, const std::string& to, bool& moved, bool& uncertain)
{
    FRESULT res = f_rename(from.c_str(), to.c_str());
    moved = res == FR_OK;
    if (moved) return true;
    if (res == FR_DISK_ERR || res == FR_INT_ERR)
    {
        uncertain = true;
        return false;
    }
    FRESULT src = f_stat(from.c_str(), nullptr);
    FRESULT dst = f_stat(to.c_str(), nullptr);
    moved = src == FR_NO_FILE && dst == FR_OK;
    uncertain = !moved && !(src == FR_OK && dst == FR_NO_FILE);
    return false;
}

bool NANDMount::ImportTitle(const NDSHeader& header, u64 length,
    const std::function<bool(u8*, u32)>& read, const DSi_TMD::TitleMetadata& tmd, bool readonly)
{
    u64 expectedLength = 0;
    for (u8 byte : tmd.Contents.ContentSize) expectedLength = (expectedLength << 8) | byte;
    auto validSave = [](u32 size) { return size == 0 || (size >= 0x200 && size <= 0x8000000 && !(size & 0x1FF)); };
    if (!header.IsDSiWare() || header.DSiTitleIDHigh != tmd.GetCategory() ||
        header.DSiTitleIDLow != tmd.GetID() || length < 0x4000 ||
        length > std::numeric_limits<FSIZE_t>::max() || expectedLength != length ||
        tmd.NumberOfContents != 0x0100 || tmd.BootContentIndex != 0 ||
        tmd.Contents.ContentIndex[0] != 0 || tmd.Contents.ContentIndex[1] != 0 ||
        !validSave(header.DSiPublicSavSize) || !validSave(header.DSiPrivateSavSize))
    {
        ImportResult = TitleImportResult::InvalidInput;
        return false;
    }

    char id[32], version[16];
    snprintf(id, sizeof(id), "%08x/%08x", tmd.GetCategory(), tmd.GetID());
    snprintf(version, sizeof(version), "%08x.app", tmd.Contents.GetVersion());
    const std::string title = std::string("0:/title/") + id;
    const std::string ticket = std::string("0:/ticket/") + id + ".tik";
    snprintf(id, sizeof(id), "%08x-%08x", tmd.GetCategory(), tmd.GetID());
    const std::string stage = std::string("0:/_install/") + id;
    FRESULT pending = f_stat(stage.c_str(), nullptr);
    if (pending == FR_OK)
    {
        ImportResult = TitleImportResult::RecoveryRequired;
        ImportRecoveryPath = stage;
        return false;
    }
    if (pending != FR_NO_FILE && pending != FR_NO_PATH) return false;
    if (!TitleDirectory("0:/_install")) return false;
    if (f_mkdir(stage.c_str()) != FR_OK) return false;
    ImportRecoveryPath = stage;

    // Five fixed installation objects, not a general filesystem transaction.
    // Existing save files (including unknown data files) are never written or
    // renamed. Missing requested saves are prepared before publishing content.
    struct Swap
    {
        std::string live, next, backup;
        bool active = true, old = false, installed = false;
    };
    std::array<Swap, 5> swaps = {{
        {title + "/data/public.sav", stage + "/public.sav", ""},
        {title + "/data/private.sav", stage + "/private.sav", ""},
        {title + "/data/banner.sav", stage + "/banner.sav", ""},
        {ticket, stage + "/ticket", stage + "/old-ticket"},
        {title + "/content", stage + "/content", stage + "/old-content"}
    }};
    std::vector<std::string> createdDirectories;
    auto fail = [&](TitleImportResult result = TitleImportResult::Failed)
    {
        ImportResult = result;
        for (auto it = createdDirectories.rbegin(); it != createdDirectories.rend(); ++it)
        {
            if (f_unlink(it->c_str()) != FR_OK)
            {
                ImportResult = TitleImportResult::CleanupPending;
                return false;
            }
        }
        if (!RemoveTitleStage(stage) || !FileFlush(Image->GetFile()))
            ImportResult = TitleImportResult::CleanupPending;
        else
            ImportRecoveryPath.clear();
        return false;
    };

    if (f_mkdir((stage + "/content").c_str()) != FR_OK) return fail();
    FF_FIL app;
    const std::string appPath = stage + "/content/" + version;
    if (f_open(&app, appPath.c_str(), FA_CREATE_NEW | FA_WRITE) != FR_OK) return fail();
    SHA1_CTX sha;
    SHA1Init(&sha);
    bool copied = true;
    u8 buffer[0x1000];
    for (u64 offset = 0; copied && offset < length;)
    {
        u32 count = static_cast<u32>(std::min<u64>(sizeof(buffer), length - offset));
        u32 written = 0;
        copied = read(buffer, count);
        // Keep the validated header and streamed content from the same input.
        if (copied && offset == 0 && memcmp(buffer, &header, sizeof(header))) copied = false;
        if (copied)
        {
            SHA1Update(&sha, buffer, count);
            copied = f_write(&app, buffer, count, &written) == FR_OK && written == count;
        }
        offset += count;
    }
    bool closed = f_close(&app) == FR_OK;
    u8 digest[20];
    SHA1Final(digest, &sha);
    if (!copied || !closed) return fail();
    if (memcmp(digest, tmd.Contents.ContentSha1Hash, sizeof(digest)))
        return fail(TitleImportResult::InvalidInput);

    // Verify the staged executable through real NAND reads before swapping. This
    // catches read failures and successful-but-corrupt writes; signatures/actual
    // title boot compatibility are not established by this hash comparison.
    if (f_open(&app, appPath.c_str(), FA_READ) != FR_OK) return fail();
    SHA1Init(&sha);
    copied = f_size(&app) == length;
    for (u64 offset = 0; copied && offset < length;)
    {
        u32 count = static_cast<u32>(std::min<u64>(sizeof(buffer), length - offset)), got = 0;
        copied = f_read(&app, buffer, count, &got) == FR_OK && got == count;
        if (copied) SHA1Update(&sha, buffer, count);
        offset += count;
    }
    closed = f_close(&app) == FR_OK;
    SHA1Final(digest, &sha);
    if (!copied || !closed || memcmp(digest, tmd.Contents.ContentSha1Hash, sizeof(digest))) return fail();

    if (!CreateTicket(swaps[3].next.c_str(), tmd.GetCategoryNoByteswap(), tmd.GetIDNoByteswap(), header.ROMVersion) ||
        !ImportFile((stage + "/content/title.tmd").c_str(), reinterpret_cast<const u8*>(&tmd), sizeof(tmd)))
        return fail();
    const u32 sizes[] = {header.DSiPublicSavSize, header.DSiPrivateSavSize, (header.AppFlags & 4) ? 0x4000U : 0U};
    for (size_t i = 0; i < 3; ++i)
    {
        auto& swap = swaps[i];
        FF_FILINFO info;
        FRESULT res = f_stat(swap.live.c_str(), &info);
        if (res == FR_OK)
        {
            if (info.fattrib & AM_DIR) return fail();
            swap.active = false;
        }
        else if (res != FR_NO_FILE && res != FR_NO_PATH) return fail();
        else if (sizes[i] == 0) swap.active = false;
        else if (i == 2)
        {
            u8 banner[0x4000] = {};
            if (!ImportFile(swap.next.c_str(), banner, sizeof(banner))) return fail();
        }
        else if (!CreateSaveFile(swap.next.c_str(), sizes[i])) return fail();
    }
    if (readonly)
    {
        for (const auto& path : {appPath, stage + "/content/title.tmd", swaps[3].next})
            if (f_chmod(path.c_str(), AM_RDO, AM_RDO) != FR_OK) return fail();
    }
    if (!FileFlush(Image->GetFile())) return fail();

    // Create only missing parent directories. Remember them so an ordinary
    // failure restores the absence of a previously uninstalled title as well.
    for (const auto& path : {std::string("0:/title"), title.substr(0, title.find_last_of('/')), title,
                            title + "/data", std::string("0:/ticket"), ticket.substr(0, ticket.find_last_of('/'))})
    {
        FF_FILINFO info;
        FRESULT res = f_stat(path.c_str(), &info);
        if (res == FR_OK) { if (!(info.fattrib & AM_DIR)) return fail(); }
        else if (res == FR_NO_FILE || res == FR_NO_PATH)
        {
            if (f_mkdir(path.c_str()) != FR_OK) return fail();
            createdDirectories.push_back(path);
        }
        else return fail();
    }

    bool uncertain = false;
    auto rollback = [&]()
    {
        if (uncertain)
        {
            ImportResult = TitleImportResult::RecoveryRequired;
            return false;
        }
        bool restored = true;
        for (auto it = swaps.rbegin(); it != swaps.rend(); ++it)
        {
            bool moved = false;
            if (it->installed)
            {
                if (!MoveTitlePath(it->live, it->next, moved, uncertain)) restored = false;
                if (uncertain) break;
                if (!moved) continue; // Never overwrite/delete an unremoved new object.
            }
            if (uncertain) break;
            if (it->old && !MoveTitlePath(it->backup, it->live, moved, uncertain)) restored = false;
            if (uncertain) break;
        }
        if (!restored || uncertain || !FileFlush(Image->GetFile()))
        {
            ImportResult = TitleImportResult::RollbackFailed;
            return false; // Preserve ALL remaining backup/stage objects.
        }
        return fail();
    };
    for (auto& swap : swaps)
    {
        if (!swap.active) continue;
        FRESULT exists = f_stat(swap.live.c_str(), nullptr);
        if (exists == FR_OK)
        {
            // A save appeared after staging: do not overwrite it.
            if (swap.backup.empty()) return rollback();
            if (!MoveTitlePath(swap.live, swap.backup, swap.old, uncertain)) return rollback();
        }
        else if (exists != FR_NO_FILE) return rollback();
        if (!MoveTitlePath(swap.next, swap.live, swap.installed, uncertain)) return rollback();
    }
    if (!FileFlush(Image->GetFile())) return rollback();

    // This is the commit point: all content, metadata, ticket and missing saves
    // are installed. Cleanup failure is a distinct installed-with-warning result.
    ImportResult = TitleImportResult::Success;
    if (!RemoveTitleStage(stage) || !FileFlush(Image->GetFile()))
        ImportResult = TitleImportResult::InstalledCleanupPending;
    else
        ImportRecoveryPath.clear();
    return true;
}

bool NANDMount::ImportTitle(const char* appfile, const DSi_TMD::TitleMetadata& tmd, bool readonly)
{
    ImportResult = TitleImportResult::Failed;
    ImportRecoveryPath.clear();
    if (!appfile) { ImportResult = TitleImportResult::InvalidInput; return false; }
    Platform::FileHandle* file = OpenLocalFile(appfile, FileMode::Read);
    if (!file) return false;
    NDSHeader header{};
    u64 length = FileLength(file);
    bool readable = FileRead(&header, sizeof(header), 1, file) == 1 && FileSeek(file, 0, FileSeekOrigin::Start);
    bool installed = false;
    if (readable)
        installed = ImportTitle(header, length, [file](u8* data, u32 count) {
            return FileRead(data, 1, count, file) == count;
        }, tmd, readonly);
    else
        ImportResult = TitleImportResult::InvalidInput;
    CloseFile(file);
    return installed;
}

bool NANDMount::ImportTitle(const u8* app, size_t appLength, const DSi_TMD::TitleMetadata& tmd, bool readonly)
{
    ImportResult = TitleImportResult::InvalidInput;
    ImportRecoveryPath.clear();
    if (!app || appLength < sizeof(NDSHeader)) return false;
    NDSHeader header;
    memcpy(&header, app, sizeof(header));
    ImportResult = TitleImportResult::Failed;
    size_t offset = 0;
    return ImportTitle(header, appLength, [&](u8* data, u32 count) {
        memcpy(data, app + offset, count);
        offset += count;
        return true;
    }, tmd, readonly);
}

void NANDMount::DeleteTitle(u32 category, u32 titleid)
{
    char fname[128];

    snprintf(fname, sizeof(fname), "0:/ticket/%08x/%08x.tik", category, titleid);
    RemoveFile(fname);

    snprintf(fname, sizeof(fname), "0:/title/%08x/%08x", category, titleid);
    RemoveDir(fname);
}

u32 NANDMount::GetTitleDataMask(u32 category, u32 titleid)
{
    u32 version;
    NDSHeader header;

    GetTitleInfo(category, titleid, version, &header, nullptr);
    if (version == 0xFFFFFFFF)
        return 0;

    u32 ret = 0;
    if (header.DSiPublicSavSize != 0)  ret |= (1 << TitleData_PublicSav);
    if (header.DSiPrivateSavSize != 0) ret |= (1 << TitleData_PrivateSav);
    if (header.AppFlags & 0x04)        ret |= (1 << TitleData_BannerSav);

    return ret;
}

bool NANDMount::ImportTitleData(u32 category, u32 titleid, int type, const char* file)
{
    char fname[128];

    switch (type)
    {
    case TitleData_PublicSav:
        snprintf(fname, sizeof(fname), "0:/title/%08x/%08x/data/public.sav", category, titleid);
        break;

    case TitleData_PrivateSav:
        snprintf(fname, sizeof(fname), "0:/title/%08x/%08x/data/private.sav", category, titleid);
        break;

    case TitleData_BannerSav:
        snprintf(fname, sizeof(fname), "0:/title/%08x/%08x/data/banner.sav", category, titleid);
        break;

    default:
        return false;
    }

    RemoveFile(fname);
    return ImportFile(fname, file);
}

bool NANDMount::ExportTitleData(u32 category, u32 titleid, int type, const char* file)
{
    char fname[128];

    switch (type)
    {
    case TitleData_PublicSav:
        snprintf(fname, sizeof(fname), "0:/title/%08x/%08x/data/public.sav", category, titleid);
        break;

    case TitleData_PrivateSav:
        snprintf(fname, sizeof(fname), "0:/title/%08x/%08x/data/private.sav", category, titleid);
        break;

    case TitleData_BannerSav:
        snprintf(fname, sizeof(fname), "0:/title/%08x/%08x/data/banner.sav", category, titleid);
        break;

    default:
        return false;
    }

    return ExportFile(fname, file);
}

void DSiFirmwareSystemSettings::UpdateHash()
{
    SHA1_CTX sha;
    SHA1Init(&sha);
    SHA1Update(&sha, &Bytes[0x88], 0x128);
    SHA1Final(Hash.data(), &sha);
}

}
