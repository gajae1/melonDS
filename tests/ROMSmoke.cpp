// SPDX-License-Identifier: GPL-3.0-or-later
// Optional local ROM smoke run. No save writes, audio/network devices or uploads.
#include <SDL.h>
#include "frontend/glad/glad.h"
#include "NDS.h"
#include "DSi.h"
#include "GPU_OpenGL.h"
#include "UTF8.h"
#include <fstream>
#include <chrono>
#include <vector>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <cstdlib>

// DSi writes go to a private RAM copy. The original NAND is never opened for writing.
namespace melonDS::Platform {
struct FileHandle { std::vector<u8> bytes; u64 position = 0; };
bool CloseFile(FileHandle* f) { delete f; return true; }
bool IsEndOfFile(FileHandle* f) { return f->position == f->bytes.size(); }
bool FileReadLine(char*, int, FileHandle*) { std::abort(); }
u64 FilePosition(FileHandle* f) { return f->position; }
bool FileSeek(FileHandle* f, s64 offset, FileSeekOrigin origin) {
    const s64 base = origin == FileSeekOrigin::Start ? 0 :
        static_cast<s64>(origin == FileSeekOrigin::End ? f->bytes.size() : f->position);
    if (offset < -base || offset > static_cast<s64>(f->bytes.size()) - base) return false;
    f->position = static_cast<u64>(base + offset); return true;
}
void FileRewind(FileHandle* f) { f->position = 0; }
u64 FileRead(void* dst, u64 size, u64 count, FileHandle* f) {
    if (!size) return 0;
    count = std::min(count, (f->bytes.size() - f->position) / size);
    if (count) std::memcpy(dst, f->bytes.data() + f->position, count * size);
    f->position += count * size; return count;
}
bool FileFlush(FileHandle*) { return true; }
u64 FileWrite(const void* src, u64 size, u64 count, FileHandle* f) {
    if (!size) return 0;
    count = std::min(count, (f->bytes.size() - f->position) / size);
    if (count) std::memcpy(f->bytes.data() + f->position, src, count * size);
    f->position += count * size; return count;
}
u64 FileWriteFormatted(FileHandle*, const char*, ...) { std::abort(); }
u64 FileLength(FileHandle* f) { return f->bytes.size(); }
}

static std::vector<melonDS::u8> Read(const char* name)
{
    std::ifstream file(melonDS::PathFromUTF8(name), std::ios::binary | std::ios::ate);
    if (!file || file.tellg() < 0 || file.tellg() > 0x20000000) throw std::runtime_error("input size/open failed");
    std::vector<melonDS::u8> data(static_cast<size_t>(file.tellg()));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(data.data()), data.size())) throw std::runtime_error("input read failed");
    return data;
}

int main(int argc, char** argv)
{
    using namespace melonDS;
    if (argc < 5 || argc > 7) { std::fprintf(stderr,"usage: ROMSmoke ROM|- software|opengl|compute frames output.ppm [DSi NAND [firmware|firmware-cart]]; BIOS read from cwd\n"); return 2; }
    const bool dsi = argc >= 6;
    const bool menu = std::strcmp(argv[1], "-") == 0;
    const bool launchCart = argc == 7 && std::strcmp(argv[6], "firmware-cart") == 0;
    const bool firmwareBoot = menu || launchCart || (argc == 7 && std::strcmp(argv[6], "firmware") == 0);
    if (argc == 7 && !firmwareBoot) return 2;
    if (menu && !dsi) return 2;
    const bool software = std::strcmp(argv[2], "software") == 0;
    const bool compute = std::strcmp(argv[2], "compute") == 0;
    if (!software && !compute && std::strcmp(argv[2], "opengl")) return 2;
    const int frames = std::stoi(argv[3]);
    if (frames < 1 || frames > 36000) return 2;
    SDL_Window* window = nullptr; SDL_GLContext context = nullptr;
    if (!software) {
        if (SDL_Init(SDL_INIT_VIDEO)) return 77;
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, compute ? 4 : 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, compute ? 3 : 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        window = SDL_CreateWindow("ROM smoke",0,0,256,384,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
        if (!window || !(context = SDL_GL_CreateContext(window)) || !gladLoadGLLoader(SDL_GL_GetProcAddress)) return 77;
    }
    {
        std::unique_ptr<NDSCart::CartCommon> cart;
        if (!menu) {
            auto rom = Read(argv[1]);
            cart = NDSCart::ParseROM(rom.data(), static_cast<u32>(rom.size()));
            if (!cart) return 3;
            std::printf("rom_unit_code=%u\n", cart->GetHeader().UnitCode);
        }
        NDSArgs args;
        // Diagnostic overrides used to separate guest boot failures from JIT behavior.
        if (std::getenv("MELONDS_SMOKE_INTERPRETER")) args.JIT.reset();
        else if (std::getenv("MELONDS_SMOKE_NO_FASTMEM")) args.JIT->FastMemory = false;
        const auto bios7 = Read("bios7.bin"), bios9 = Read("bios9.bin"), firmware = Read("firmware.bin");
        if (!dsi && bios7.size() == args.ARM7BIOS->size() && bios9.size() == args.ARM9BIOS->size()) {
            std::memcpy(args.ARM7BIOS->data(), bios7.data(), args.ARM7BIOS->size());
            std::memcpy(args.ARM9BIOS->data(), bios9.data(), args.ARM9BIOS->size());
            std::puts("bios=external-DS");
        } else if (!dsi) {
            // DSi 64KiB images are not DS 16KiB/4KiB images. Do not truncate them.
            std::fprintf(stderr,"DS BIOS size mismatch; using built-in FreeBIOS for DS direct boot\n");
        }
        args.Firmware = Firmware(firmware.data(), static_cast<u32>(firmware.size()));
        if (dsi && std::filesystem::exists("biosnds7.bin") && std::filesystem::exists("biosnds9.bin")) {
            const auto ntr7 = Read("biosnds7.bin"), ntr9 = Read("biosnds9.bin");
            if (ntr7.size() != ARM7BIOSSize || ntr9.size() != ARM9BIOSSize) return 11;
            std::memcpy(args.ARM7BIOS->data(), ntr7.data(), ntr7.size());
            std::memcpy(args.ARM9BIOS->data(), ntr9.data(), ntr9.size());
            std::puts("compatibility_bios=external-DS");
        }
        std::unique_ptr<NDS> nds;
        if (dsi) {
            DSiArgs dsiargs{std::move(args)};
            if (bios7.size() != DSiBIOSSize || bios9.size() != DSiBIOSSize) return 11;
            std::memcpy(dsiargs.ARM7iBIOS->data(), bios7.data(), bios7.size());
            std::memcpy(dsiargs.ARM9iBIOS->data(), bios9.data(), bios9.size());
            dsiargs.NANDImage.emplace(new Platform::FileHandle{Read(argv[5])}, &bios7[0x8308]);
            if (!*dsiargs.NANDImage) return 12;
            {
                DSi_NAND::NANDMount mount(*dsiargs.NANDImage);
                DSi_NAND::DSiFirmwareSystemSettings settings{};
                if (!mount || !mount.ReadUserData(settings)) return 13;
            }
            std::puts("console=DSi bios=external-DSi nand=private-memory-copy dsp=LLE");
            nds = std::make_unique<DSi>(std::move(dsiargs));
        } else {
            nds = std::make_unique<NDS>(std::move(args));
        }
        nds->SetNDSCart(std::move(cart)); nds->Reset();
        if (!software) {
            nds->SetRenderer(std::make_unique<GLRenderer>(*nds, compute));
            if (!dynamic_cast<GLRenderer*>(&nds->GetRenderer())) return 5;
        }
        RendererSettings settings{1,false,false,false};
        nds->GetRenderer().SetRenderSettings(settings);
        while (nds->GetRenderer().NeedsShaderCompile()) { int step, total; nds->GetRenderer().ShaderCompileStep(step,total); }
        if (!firmwareBoot) nds->SetupDirectBoot(UTF8ToString(PathFromUTF8(argv[1]).filename().u8string()));
        nds->Start();
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < frames; ++i) {
            // Acknowledge the DSi health screen; subsequent menu state is captured as-is.
            if (firmwareBoot && i == 1200) nds->TouchScreen(128, 96);
            if (firmwareBoot && i == 1202) nds->ReleaseScreen();
            // Move to the left end of the DSi menu, where the cartridge slot is shown.
            if (launchCart && i >= 1800 && i < 2200) {
                if (i % 20 == 0) nds->SetKeyMask(0xFFF & ~(1u << 5));
                if (i % 20 == 2) nds->SetKeyMask(0xFFF);
            }
            if (launchCart && i == 2400) nds->SetKeyMask(0xFFF & ~1u);
            if (launchCart && i == 2402) nds->SetKeyMask(0xFFF);
            nds->RunFrame();
            if (!nds->IsRunning()) { std::fprintf(stderr,"stopped frame=%d\n",i); return 6; }
        }
        if (!software) glFinish();
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        void *top = nullptr, *bottom = nullptr;
        nds->GetRenderer().GetFramebuffers(&top,&bottom);
        std::vector<u32> pixels(256*384);
        if (software) {
            if (!top || !bottom) return 7;
            std::memcpy(pixels.data(),top,256*192*4);
            std::memcpy(pixels.data()+256*192,bottom,256*192*4);
        } else {
            if (!top) return 7;
            GLuint fb; glGenFramebuffers(1,&fb); glBindFramebuffer(GL_READ_FRAMEBUFFER,fb);
            for (int layer=0;layer<2;++layer) {
                glFramebufferTextureLayer(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,*static_cast<GLuint*>(top),0,layer);
                glReadBuffer(GL_COLOR_ATTACHMENT0);
                if(glCheckFramebufferStatus(GL_READ_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) return 8;
                glReadPixels(0,0,256,192,GL_BGRA,GL_UNSIGNED_BYTE,pixels.data()+layer*256*192);
            }
            glDeleteFramebuffers(1,&fb);
            if (glGetError()!=GL_NO_ERROR) return 9;
        }
        std::ofstream image(PathFromUTF8(argv[4]),std::ios::binary);
        image << "P6\n256 384\n255\n";
        u64 digest=0;
        for(auto pixel:pixels) { digest=digest*31+pixel; const char rgb[]={char(pixel>>16),char(pixel>>8),char(pixel)}; image.write(rgb,3); }
        if(!image) return 10;
        std::printf("renderer=%s frames=%d seconds=%.3f unthrottled_fps=%.3f framebuffer_digest=%llu running=1\n",argv[2],frames,seconds,frames/seconds,(unsigned long long)digest);
        if (std::all_of(pixels.begin(), pixels.end(), [&](u32 p) { return p == pixels.front(); })) {
            std::fprintf(stderr, "uniform-frame: guest execution alone does not prove successful boot\n");
            return 14;
        }
    }
    if(context) SDL_GL_DeleteContext(context);
    if(window) SDL_DestroyWindow(window);
    SDL_Quit(); return 0;
}
