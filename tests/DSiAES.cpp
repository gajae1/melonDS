// SPDX-License-Identifier: GPL-3.0-or-later
// Real DSi ARM7 MMIO, NDMA and full savestates. No CPU boot or private inputs.
// Hardware format: original GBATEK research, DSi AES I/O Ports:
// https://problemkaputt.de/gbatek.htm#dsiaesioports
// Independent CCM outputs: Google/C2SP Wycheproof, selected tcIds below:
// https://github.com/C2SP/wycheproof/blob/e0df04e0c033f2d25c5051dd06230336c7822358/testvectors_v1/aes_ccm_test.json
// DSi does not prepend the CCM AAD length; the caller supplies formatted AAD.
// CTR control: NIST SP 800-38A, F.5.1.
#include "DSi.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace melonDS;

namespace
{
constexpr u32 Cnt = 0x04004400, BlkCnt = Cnt + 4, Input = Cnt + 8, Output = Cnt + 12;
constexpr u32 IV = Cnt + 0x20, MAC = Cnt + 0x30, Key = Cnt + 0x40;
constexpr u32 Busy = 1u << 31, IRQEnable = 1u << 30, Verified = 1u << 21;
constexpr u32 RegisterTag = 1u << 20, ApplyKey = 1u << 24, IF2 = 0x0400021C;
constexpr u32 AESIRQ = 1u << IRQ2_DSi_AES;
using Bytes = std::vector<u8>;
using Words = std::vector<u32>;

struct Vector
{
    unsigned Id;
    const char *Key, *Nonce, *AAD, *Plain, *Cipher, *Tag;
};

const Vector Vectors[] = {
    {4, "ffdf4228361ea1f8165852136b3480f7", "0e1666f2dc652f7708fb8f0d", "",
     "25b12e28ac0ef6ead0226a3b2288c800",
     "aaf596fa5b00aaac27700146aec932a9", "848b6735d32c96e4a0532bcdfaf33582"},
    {9, "e12260fcd355a51a0d01bb1f6fa538c2", "5dfc37366f5688275147d3f9", "",
     "d902deeab175c008329a33bfaccd5c0eb3a6a152a1510e7db04fa0aff7ce4288530db6a80fa7fea582aa7d46d7d56e708d2bb0c5edd3d26648d336c3620ea55e",
     "0de44fe54b84359365054a6997478f87b6b761d127a651f7b8003d25e762f7f81cf84b3a471a9377bb388c74c08be894eff10e46365bf76820b7168623966be6", "bb8e261879d6f639aa42d2d50ed750b8"},
    {12, "9415f925bcb41dc25e86c826dbc8bf68", "bdffaa763b916ff0ee3f3ce4", "705d676cd8a94451",
     "feb36167eafc02c8e2bd6e13817686ba",
     "08db327a88be7b48f430fd7bfccdf502", "b7c249f810adacf99abded1f3b9130f2"},
    {344, "71466dc3046b1e6c0838ba6c9ef41e79", "2928095bd7962e9e6024a2b9", "a617cce74d0439900597cb3ddcfc25fb",
     "",
     "", "383f8abc"},
    {345, "3cc93804e2d699619278a941389cec3c", "c775dda314af64c310a7c1d3", "",
     "124ce71e08c1324f916570d533032919",
     "f246754cd32a9960d3d5e5352f1d73c7", "60dbd676"},
    {353, "63c747be2f3069d50015f69dbae09876", "bc2c940525e514409815ab19", "",
     "ad5ca70a325363c34b2f3d5a8576b964",
     "acb62f8c4781279d5c81ccaee4f61ebe", "cbbca0326950"},
    {361, "22ed64b5b94a3c4116d02b4fbd4e5881", "f498fd65dab234520de52920", "",
     "94b03b07772b70562bc729505b4ad426",
     "4c4dfe9711b320264f3a57ecdcd59850", "b13aea2980767fd7"},
    {369, "b8bae01260ced6194ef8df722d659be6", "71d10b7cbbbecb843e678ab5", "",
     "387c0324cd47d3f22cc9d968a72e434d",
     "0c36e303e295a289bb134740e21a6664", "d3587e2186553fd9d409"},
    {377, "f363f1a7d33c96949fd08f440cfba000", "67b92007f57b83fd9f3ee6fa", "",
     "a651d2ca4b16980b0e4a7a10c75c47ed",
     "20c2a2f18d0753acd36e204985149528", "4a4422d3b99c8d77dbde2ab2"},
    {385, "1e70de0cba8f8848dbc8dd9cfa53c161", "d4e677bdb04bf935d130ce15", "",
     "7102b7710b1db1a0748474f8e37b6dd8",
     "55dfe0e88c81bfc561975dfabaa21a12", "024e3bf1985a7f7eccdaa0ee2a18"},
};

Bytes Hex(std::string_view s)
{
    Bytes b;
    for (size_t i = 0; i < s.size(); i += 2)
        b.push_back(static_cast<u8>(std::stoul(std::string(s.substr(i, 2)), nullptr, 16)));
    return b;
}

// Published vectors use network byte order. DSi transfers the least significant
// 32-bit word first, without any configurable 3DS word/byte-order bits.
Words ToWords(const Bytes& bytes)
{
    Words words;
    for (size_t block = 0; block < bytes.size(); block += 16)
        for (int offset : {12, 8, 4, 0})
        {
            u32 value = 0;
            for (unsigned byte = 0; byte < 4; ++byte)
                value = (value << 8) | bytes.at(block + offset + byte);
            words.push_back(value);
        }
    return words;
}

Words TagWords(const Vector& v)
{
    auto tag = Hex(v.Tag);
    tag.resize(16); // Short tags still occupy four FIFO words; low bytes are zero.
    return ToWords(tag);
}

Words AADWords(const Vector& v)
{
    auto data = Hex(v.AAD);
    if (data.empty()) return {};
    const unsigned length = data.size();
    data.insert(data.begin(), {static_cast<u8>(length >> 8), static_cast<u8>(length)});
    data.resize((data.size() + 15) / 16 * 16);
    return ToWords(data);
}

struct Fixture
{
    std::unique_ptr<DSi> Core;

    Fixture()
    {
        DSiArgs args;
        args.JIT.reset();
        Core = std::make_unique<DSi>(std::move(args));
        Reset();
    }

    void Reset() { Core->CurCPU = 1; Core->Reset(); }
    u32 Read(u32 addr) { return Core->ARM7Read32(addr); }
    void Write(u32 addr, u32 value) { Core->ARM7Write32(addr, value); }
    void Require(bool ok, const char* message)
    {
        if (!ok)
        {
            std::fprintf(stderr, "FAIL: %s (AES_CNT=%08x IF2=%08x)\n", message, Read(Cnt), Read(IF2));
            throw std::runtime_error(message);
        }
    }
    void Registers(u32 addr, const Words& words)
    {
        for (u32 word : words) { Write(addr, word); addr += 4; }
    }
    void Feed(const Words& words) { for (u32 word : words) Write(Input, word); }
    void Drain(const Words& expected)
    {
        for (u32 word : expected)
        {
            Require(((Read(Cnt) >> 5) & 31) != 0, "missing output word");
            Require(Read(Output) == word, "plaintext/ciphertext differs from independent vector");
        }
    }
    void Waiting()
    {
        Require((Read(Cnt) & (Busy | Verified)) == Busy, "must wait for complete tag, with result invalid/busy");
        Require(!(Read(IF2) & AESIRQ), "AES completion IRQ before complete tag");
    }
    void Done(bool valid, bool irq = true)
    {
        Require((Read(Cnt) & (Busy | Verified)) == (valid ? Verified : 0), "completion/authentication result");
        Require((Read(Cnt) & 31) == 0, "tag/payload left in input FIFO");
        Require(bool(Read(IF2) & AESIRQ) == irq, "completion IRQ enable/result");
    }
    void Start(const Vector& v, u32 mode = 0, bool irq = true, int tagCode = -1)
    {
        Registers(Key, ToWords(Hex(v.Key)));
        Registers(IV, ToWords(Hex(std::string("00000000") + v.Nonce)));
        Write(IF2, AESIRQ);
        Write(BlkCnt, (Hex(v.Plain).size() / 16 << 16) | (AADWords(v).size() / 4));
        if (tagCode < 0) tagCode = Hex(v.Tag).size() / 2 - 1;
        Write(Cnt, Busy | ApplyKey | (irq ? IRQEnable : 0) | (tagCode << 16) | mode);
    }
    void Payload(const Vector& v)
    {
        Feed(AADWords(v));
        Feed(ToWords(Hex(v.Cipher)));
    }
    void Load(Savestate& saved)
    {
        Savestate state(saved.Buffer(), saved.Length(), false);
        Require(Core->NDS::DoSavestate(&state) && !state.Error, "full DSi savestate load");
    }
    void Save(Savestate& state)
    {
        Require(Core->NDS::DoSavestate(&state) && !state.Error, "full DSi savestate save");
    }
};

void FIFOTests(Fixture& f)
{
    for (const auto& v : Vectors)
    {
        const auto plain = ToWords(Hex(v.Plain));
        // Good tag, wrong tag byte, reversed word order, nonzero padding.
        for (unsigned damage = 0; damage < (Hex(v.Tag).size() < 16 ? 4u : 3u); ++damage)
        {
            f.Reset();
            if (damage == 1) f.Registers(MAC, TagWords(v));
            f.Start(v, 0, damage != 1);
            f.Payload(v);
            f.Waiting();
            auto tag = TagWords(v);
            if (damage == 1) tag[3] ^= 0x01000000;
            if (damage == 2) std::reverse(tag.begin(), tag.end());
            if (damage == 3) tag[0] |= 1;
            for (unsigned i = 0; i < 4; ++i)
            {
                f.Write(Input, tag[i]);
                if (i != 3) f.Waiting();
            }
            f.Done(damage == 0, damage != 1);
            f.Drain(plain);
            f.Write(IF2, AESIRQ);
            f.Core->AES.CheckInputDMA(); // NDMA completion callback after AES completion.
            f.Require(!(f.Read(IF2) & AESIRQ), "completed AES raised IRQ again");
        }
        std::printf("fifo/wycheproof-%u: PASS\n", v.Id);
    }
    // Size encoding 0 is the documented alias for a 4-byte MAC.
    f.Reset();
    f.Start(Vectors[4], 0, true, 0);
    f.Payload(Vectors[4]);
    f.Feed(TagWords(Vectors[4]));
    f.Done(true);
    f.Drain(ToWords(Hex(Vectors[4].Plain)));
    // A fully buffered request must also start without an additional FIFO write.
    f.Reset();
    f.Feed(ToWords(Hex(Vectors[0].Cipher)));
    f.Feed(TagWords(Vectors[0]));
    f.Write(Input, 0xDEADC0DE);
    f.Start(Vectors[0]);
    f.Require((f.Read(Cnt) & (Busy | Verified | 31)) == (Verified | 1),
              "prefilled request must consume exactly one tag and retain trailing word");
    f.Require(f.Read(IF2) & AESIRQ, "prefilled request completion IRQ");
    f.Drain(ToWords(Hex(Vectors[0].Plain)));
}

const char* CTRPlain =
    "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
    "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710";
const char* CTRCipher =
    "874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff"
    "5ae4df3edbd5d35e5b4f09020db03eab1e031dda2fbe03d1792170a0f3009cee";

void FillCTR(Fixture& f, unsigned mode)
{
    f.Registers(Key, ToWords(Hex("2b7e151628aed2a6abf7158809cf4f3c")));
    f.Registers(IV, ToWords(Hex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff")));
    f.Write(BlkCnt, 4 << 16);
    f.Write(Cnt, Busy | ApplyKey | IRQEnable | (mode << 28));
    f.Feed(ToWords(Hex(CTRPlain)));
    f.Done(false);
}

void Controls(Fixture& f)
{
    for (unsigned mode : {2u, 3u})
    {
        f.Reset();
        FillCTR(f, mode);
        f.Drain(ToWords(Hex(CTRCipher)));
    }
    unsigned failures = 0;
    for (const auto& v : Vectors)
    for (int tagCode : {-1, 0})
    {
        // Exercise the size-code-0 alias with the same published four-byte tag.
        if (tagCode == 0 && v.Id != 345) continue;
        try
        {
            // Good tag, wrong authentication byte, nonzero short-tag padding.
            for (unsigned damage = 0; damage < (Hex(v.Tag).size() < 16 ? 3u : 2u); ++damage)
            {
                f.Reset();
                auto tag = TagWords(v);
                if (damage == 1) tag[3] ^= 0x01000000;
                if (damage == 2) tag[0] |= 1;
                f.Registers(MAC, tag);
                f.Start(v, RegisterTag, true, tagCode);
                f.Payload(v);
                f.Done(damage == 0);
                f.Drain(ToWords(Hex(v.Plain)));
            }
            std::printf("controls/register/wycheproof-%u code=%d: PASS\n", v.Id, tagCode);
        }
        catch (const std::exception&)
        {
            ++failures;
            std::fprintf(stderr, "controls/register/wycheproof-%u code=%d: FAIL\n", v.Id, tagCode);
        }
        // Run independently of register verification so either defect is visible.
        try
        {
            f.Reset();
            f.Start(v, 1u << 28, true, tagCode);
            f.Feed(AADWords(v));
            f.Feed(ToWords(Hex(v.Plain)));
            f.Done(false);
            f.Drain(ToWords(Hex(v.Cipher)));
            f.Drain(TagWords(v)); // All four words, including mandatory zero padding.
            f.Require(((f.Read(Cnt) >> 5) & 31) == 0, "encrypt extra output");
            std::printf("controls/encrypt/wycheproof-%u code=%d: PASS\n", v.Id, tagCode);
        }
        catch (const std::exception&)
        {
            ++failures;
            std::fprintf(stderr, "controls/encrypt/wycheproof-%u code=%d: FAIL\n", v.Id, tagCode);
        }
    }
    f.Require(failures == 0, "register/encrypt vector failures");
    std::puts("controls/register-good-bad-padding encrypt-padded-deferred CTR2-3: PASS");
}

void SnapshotsAndBackpressure(Fixture& f)
{
    const auto& v = Vectors[0];
    const auto cipher = ToWords(Hex(v.Cipher)), tag = TagWords(v);
    for (unsigned prefix : {0u, 1u, 3u})
    {
        f.Reset();
        f.Start(v);
        f.Feed(cipher);
        for (unsigned i = 0; i < prefix; ++i) f.Write(Input, tag[i]);
        f.Waiting();
        Savestate saved;
        f.Save(saved);
        for (unsigned replay = 0; replay < 2; ++replay)
        {
            if (replay) f.Load(saved);
            f.Waiting();
            for (unsigned i = prefix; i < 4; ++i) f.Write(Input, tag[i]);
            f.Done(true);
            f.Drain(ToWords(Hex(v.Plain)));
        }
    }
    // A payload block split across a snapshot must still consume four words.
    f.Reset();
    f.Start(v);
    f.Write(Input, cipher[0]);
    f.Write(Input, cipher[1]);
    Savestate partial;
    f.Save(partial);
    f.Reset();
    f.Load(partial);
    f.Require((f.Read(Cnt) & 0x3FF) == 2, "partial payload snapshot FIFO counts");
    f.Write(Input, cipher[2]);
    f.Write(Input, cipher[3]);
    f.Feed(tag);
    f.Done(true);
    f.Drain(ToWords(Hex(v.Plain)));

    // Leave four prior CTR blocks unread, filling RDFIFO. A subsequent CCM
    // request fills WRFIFO and stalls until those prior results are consumed.
    f.Reset();
    FillCTR(f, 2);
    const auto& longVector = Vectors[1];
    f.Start(longVector);
    f.Payload(longVector);
    f.Waiting();
    f.Require((f.Read(Cnt) & 0x3FF) == (16 | (16 << 5)), "input/output backpressure absent");
    Savestate stalled;
    f.Save(stalled);
    for (unsigned replay = 0; replay < 2; ++replay)
    {
        if (replay) f.Load(stalled);
        f.Drain(ToWords(Hex(CTRCipher)));
        f.Waiting();
        f.Require((f.Read(Cnt) & 0x3FF) == (16 << 5), "buffered payload was not resumed");
        f.Feed(TagWords(longVector));
        f.Done(true); // Authentication does not need space in RDFIFO.
        f.Drain(ToWords(Hex(longVector.Plain)));
    }
    // A FIFO-supplied tag must not overwrite the separate write-only MAC bank.
    f.Registers(MAC, TagWords(v));
    f.Start(longVector);
    f.Payload(longVector);
    f.Feed(TagWords(longVector));
    f.Drain(ToWords(Hex(longVector.Plain)));
    f.Start(v, RegisterTag);
    f.Payload(v);
    f.Done(true);
    std::puts("snapshot/partial-payload partial-tag full-fifos MAC-bank: PASS");
}

void NDMA(Fixture& f)
{
    constexpr u32 Source = 0x02020000, Dest = 0x02021000;
    const auto& v = Vectors[0];
    auto input = ToWords(Hex(v.Cipher));
    const auto tag = TagWords(v);
    input.insert(input.end(), tag.begin(), tag.end());
    f.Registers(Source, input);
    auto setup = [&](unsigned channel, u32 src, u32 dst, u32 total, u32 control) {
        const u32 reg = 0x04004104 + channel * 0x1C;
        f.Write(reg, src);
        f.Write(reg + 4, dst);
        f.Write(reg + 8, total);
        f.Write(reg + 12, 4); // Match AES logical request: four words.
        f.Write(reg + 16, 0);
        f.Write(reg + 24, Busy | IRQEnable | (2u << 16) | control);
    };
    setup(0, Source, Input, 8, (0xAu << 24) | (2u << 10));
    setup(1, Output, Dest, 4, (0xBu << 24) | (2u << 13));
    // WRFIFO=4 words, RDFIFO=4 words. Channels are armed before AES starts.
    f.Start(v, 3u << 12);
    auto step = [&] {
        f.Core->ARM7Target = f.Core->ARM7Timestamp + 1;
        f.Core->RunNDMAs(1);
    };
    for (unsigned i = 0; i < 4; ++i) step();
    f.Waiting();
    f.Require(f.Core->NDMAs[4].IsRunning(), "tag DMA request missing after payload block");
    Savestate boundary;
    f.Save(boundary);
    for (unsigned replay = 0; replay < 2; ++replay)
    {
        if (replay) f.Load(boundary);
        for (unsigned i = 0; i < 3; ++i) { step(); f.Waiting(); }
        step();
        f.Done(true);
        for (unsigned i = 0; i < 8 && f.Core->NDMAsRunning(1); ++i) step();
        const auto plain = ToWords(Hex(v.Plain));
        for (unsigned i = 0; i < plain.size(); ++i)
            f.Require(f.Read(Dest + i * 4) == plain[i], "NDMA output differs from public plaintext");
        f.Require(!(f.Core->NDMAs[4].Cnt & Busy) && !(f.Core->NDMAs[5].Cnt & Busy), "NDMA did not complete");
        f.Require(!f.Core->NDMAsRunning(1), "NDMA still running after tag and payload");
        f.Require((f.Read(Cnt) & 0x3FF) == 0, "NDMA left FIFO data behind");
        f.Require((f.Read(0x04000214) & (3u << 28)) == (3u << 28), "NDMA completion IRQs");
    }
    std::puts("ndma/payload-tag-boundary snapshot output IRQs: PASS");
}
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    try
    {
        Fixture fixture;
        const std::string_view group = argv[1];
        if (group == "fifo") FIFOTests(fixture);
        else if (group == "controls") Controls(fixture);
        else if (group == "snapshot-backpressure") SnapshotsAndBackpressure(fixture);
        else if (group == "ndma") NDMA(fixture);
        else return 2;
        std::printf("dsi-aes-%s: PASS\n", argv[1]);
        return 0;
    }
    catch (const std::exception&) { return 1; }
}
