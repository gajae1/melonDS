// SPDX-License-Identifier: GPL-3.0-or-later
#include "external_display.h"
#include "context.h"
#include "Vulkan/ExternalDisplay.h"
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

// Self-contained GL types/enums: no glad or platform headers are needed here, all
// entry points come from Context::GetProcAddress.
namespace GL {
namespace {
using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLubyte = unsigned char;
using GLuint64 = unsigned long long;
#ifdef _WIN32
#define MELONDS_GLEXT_CALL __stdcall
#else
#define MELONDS_GLEXT_CALL
#endif

constexpr GLenum kTexture2D = 0x0DE1, kTextureBinding2D = 0x8069, kTextureMagFilter = 0x2800,
                 kTextureMinFilter = 0x2801, kTextureWrapS = 0x2802, kTextureWrapT = 0x2803,
                 kNearest = 0x2600, kClampToEdge = 0x812F, kR32UI = 0x8236, kExtensions = 0x1F03,
                 kNumExtensions = 0x821D, kTilingExt = 0x9580, kDedicatedMemoryExt = 0x9581,
                 kNumTilingTypes = 0x9582, kTilingTypes = 0x9583, kOptimalTiling = 0x9584,
                 kHandleOpaqueWin32 = 0x9587, kLayoutGeneral = 0x958D, kNumDeviceUuids = 0x9596,
                 kDeviceUuid = 0x9597, kDriverUuid = 0x9598;
constexpr GLint kMaxDevices = 64, kMaxTilings = 32;

struct Functions {
  GLenum(MELONDS_GLEXT_CALL* getError)();
  void(MELONDS_GLEXT_CALL* getIntegerv)(GLenum, GLint*);
  const GLubyte*(MELONDS_GLEXT_CALL* getStringi)(GLenum, GLuint);
  void(MELONDS_GLEXT_CALL* getInternalformativ)(GLenum, GLenum, GLenum, GLsizei, GLint*);
  void(MELONDS_GLEXT_CALL* genTextures)(GLsizei, GLuint*);
  void(MELONDS_GLEXT_CALL* deleteTextures)(GLsizei, const GLuint*);
  void(MELONDS_GLEXT_CALL* bindTexture)(GLenum, GLuint);
  void(MELONDS_GLEXT_CALL* texParameteri)(GLenum, GLenum, GLint);
  void(MELONDS_GLEXT_CALL* getUnsignedBytev)(GLenum, GLubyte*);
  void(MELONDS_GLEXT_CALL* getUnsignedBytei)(GLenum, GLuint, GLubyte*);
  void(MELONDS_GLEXT_CALL* createMemoryObjects)(GLsizei, GLuint*);
  void(MELONDS_GLEXT_CALL* deleteMemoryObjects)(GLsizei, const GLuint*);
  void(MELONDS_GLEXT_CALL* memoryObjectParameteriv)(GLuint, GLenum, const GLint*);
  void(MELONDS_GLEXT_CALL* importMemoryWin32)(GLuint, GLuint64, GLenum, void*);
  void(MELONDS_GLEXT_CALL* texStorageMem2D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLuint64);  // optional
  void(MELONDS_GLEXT_CALL* textureStorageMem2D)(GLuint, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLuint64);  // DSA fallback
  void(MELONDS_GLEXT_CALL* genSemaphores)(GLsizei, GLuint*);
  void(MELONDS_GLEXT_CALL* deleteSemaphores)(GLsizei, const GLuint*);
  void(MELONDS_GLEXT_CALL* importSemaphoreWin32)(GLuint, GLenum, void*);
  void(MELONDS_GLEXT_CALL* waitSemaphore)(GLuint, GLuint, const GLuint*, GLuint, const GLuint*, const GLenum*);
  void(MELONDS_GLEXT_CALL* signalSemaphore)(GLuint, GLuint, const GLuint*, GLuint, const GLuint*, const GLenum*);
};

template <class T> void Load(Context& context, T& fn, const char* name, bool required = true) {
  fn = reinterpret_cast<T>(context.GetProcAddress(name));
  if (!fn && required) throw std::runtime_error(std::string("External display: missing GL entry point ") + name);
}
} // namespace

using melonDS::Vulkan::Device;

struct ExternalDisplay::State : std::enable_shared_from_this<State> {
  using ImagePair = std::array<std::shared_ptr<Device::Image>, 2>;
  struct Slot {
    ImagePair images;       // retained until the GL objects below are deleted
    GLuint memory[2]{};
    GLuint texture[2]{};
    unsigned stamp = 0;
  };

  Functions gl{};
  // Destruction order (reverse of declaration): session, image cache, device.
  std::shared_ptr<Device> device;
  Slot slots[2];
  std::unique_ptr<melonDS::Vulkan::ExternalDisplay> session;
  GLuint readySemaphore = 0, returnedSemaphore = 0;
  int active = -1;          // slot in the current handoff
  unsigned clock = 0;
  bool handoff = false;     // core Release done, Acquire not yet
  bool broken = false;      // unusable, ordinary cleanup still valid
  bool ambiguous = false;   // quarantined: never touch GL/Vulkan objects again
  // Preallocated so quarantining cannot fail: holds a self reference forever.
  std::unique_ptr<std::shared_ptr<State>> keep = std::make_unique<std::shared_ptr<State>>();

  State(Context& context, std::shared_ptr<Device> dev) : device(std::move(dev)) {
    try {
      Init(context);
    } catch (...) {  // ~State does not run for a failed constructor; nothing was released
      Cleanup();
      throw;
    }
  }
  ~State() {
    if (!ambiguous) Cleanup();
  }

  void ClearErrors() {
    for (int i = 0; i < 32 && gl.getError() != 0; ++i) {}
  }
  // First pending error (all drained), 0 when none.
  GLenum TakeError() {
    GLenum first = gl.getError();
    for (int i = 0; first && i < 32 && gl.getError() != 0; ++i) {}
    return first;
  }
  void CheckGL(const char* where) {
    if (GLenum e = TakeError())
      throw std::runtime_error(std::string("External display: GL error 0x") + std::to_string(e) + " at " + where);
  }

  void Init(Context& context) {
    if (!device) throw std::runtime_error("External display: no Vulkan device");
    if (!device->ExternalImagesSupported())
      throw std::runtime_error("External display: Vulkan device has no external image support");
    LoadFunctions(context);
    ClearErrors();
    CheckExtensions();
    CheckUuids();
    CheckTiling();
    session = std::make_unique<melonDS::Vulkan::ExternalDisplay>(device);
    ImportSemaphore(readySemaphore, session->ReadyHandle(), "Ready semaphore import");
    ImportSemaphore(returnedSemaphore, session->ReturnedHandle(), "Returned semaphore import");
  }

  void LoadFunctions(Context& c) {
    Load(c, gl.getError, "glGetError");
    Load(c, gl.getIntegerv, "glGetIntegerv");
    Load(c, gl.getStringi, "glGetStringi");
    Load(c, gl.getInternalformativ, "glGetInternalformativ");
    Load(c, gl.genTextures, "glGenTextures");
    Load(c, gl.deleteTextures, "glDeleteTextures");
    Load(c, gl.bindTexture, "glBindTexture");
    Load(c, gl.texParameteri, "glTexParameteri");
    Load(c, gl.getUnsignedBytev, "glGetUnsignedBytevEXT");
    Load(c, gl.getUnsignedBytei, "glGetUnsignedBytei_vEXT");
    Load(c, gl.createMemoryObjects, "glCreateMemoryObjectsEXT");
    Load(c, gl.deleteMemoryObjects, "glDeleteMemoryObjectsEXT");
    Load(c, gl.memoryObjectParameteriv, "glMemoryObjectParameterivEXT");
    Load(c, gl.importMemoryWin32, "glImportMemoryWin32HandleEXT");
    Load(c, gl.texStorageMem2D, "glTexStorageMem2DEXT", false);
    Load(c, gl.textureStorageMem2D, "glTextureStorageMem2DEXT", false);
    if (!gl.texStorageMem2D && !gl.textureStorageMem2D)
      throw std::runtime_error("External display: missing GL entry point glTexStorageMem2DEXT/glTextureStorageMem2DEXT");
    Load(c, gl.genSemaphores, "glGenSemaphoresEXT");
    Load(c, gl.deleteSemaphores, "glDeleteSemaphoresEXT");
    Load(c, gl.importSemaphoreWin32, "glImportSemaphoreWin32HandleEXT");
    Load(c, gl.waitSemaphore, "glWaitSemaphoreEXT");
    Load(c, gl.signalSemaphore, "glSignalSemaphoreEXT");
  }

  void CheckExtensions() {
    static const char* const required[4] = {"GL_EXT_memory_object", "GL_EXT_memory_object_win32",
                                            "GL_EXT_semaphore", "GL_EXT_semaphore_win32"};
    bool found[4] = {};
    GLint count = 0;
    gl.getIntegerv(kNumExtensions, &count);
    for (GLint i = 0; i < count; ++i) {
      const char* name = reinterpret_cast<const char*>(gl.getStringi(kExtensions, static_cast<GLuint>(i)));
      if (!name) continue;
      for (int r = 0; r < 4; ++r) found[r] |= std::strcmp(name, required[r]) == 0;
    }
    CheckGL("extension query");
    for (int r = 0; r < 4; ++r)
      if (!found[r]) throw std::runtime_error(std::string("External display: missing ") + required[r]);
  }

  void CheckUuids() {
    GLint devices = 0;
    gl.getIntegerv(kNumDeviceUuids, &devices);
    CheckGL("device UUID count");
    if (devices > kMaxDevices) devices = kMaxDevices;
    bool deviceMatch = false;
    for (GLint i = 0; i < devices && !deviceMatch; ++i) {
      GLubyte uuid[16] = {};
      gl.getUnsignedBytei(kDeviceUuid, static_cast<GLuint>(i), uuid);
      deviceMatch = std::memcmp(uuid, device->DeviceUUID().data(), 16) == 0;
    }
    GLubyte driver[16] = {};
    gl.getUnsignedBytev(kDriverUuid, driver);
    CheckGL("UUID query");
    if (!deviceMatch) throw std::runtime_error("External display: no GL device matches the Vulkan device UUID");
    if (std::memcmp(driver, device->DriverUUID().data(), 16) != 0)
      throw std::runtime_error("External display: GL driver UUID differs from the Vulkan driver UUID");
  }

  void CheckTiling() {
    GLint count = 0;
    gl.getInternalformativ(kTexture2D, kR32UI, kNumTilingTypes, 1, &count);
    GLint tilings[kMaxTilings] = {};
    bool optimal = false;
    if (count > 0 && count <= kMaxTilings) {
      gl.getInternalformativ(kTexture2D, kR32UI, kTilingTypes, count, tilings);
      for (GLint i = 0; i < count; ++i) optimal |= static_cast<GLenum>(tilings[i]) == kOptimalTiling;
    }
    CheckGL("tiling query");
    if (!optimal) throw std::runtime_error("External display: GL R32UI does not support OPTIMAL tiling");
  }

  void ImportSemaphore(GLuint& semaphore, void* handle, const char* where) {
    if (!handle) throw std::runtime_error("External display: core session returned no semaphore handle");
    gl.genSemaphores(1, &semaphore);
    gl.importSemaphoreWin32(semaphore, kHandleOpaqueWin32, handle);
    CheckGL(where);
  }

  // Ordinary cleanup (context current, no handoff in flight): GL imports first,
  // then the core session, then the image references.
  void Cleanup() noexcept {
    if (!gl.getError) return;  // constructor failed before functions were loaded
    for (int i = 0; i < 2; ++i) DeleteSlotGL(slots[i]);
    if (readySemaphore) gl.deleteSemaphores(1, &readySemaphore);
    if (returnedSemaphore) gl.deleteSemaphores(1, &returnedSemaphore);
    readySemaphore = returnedSemaphore = 0;
    ClearErrors();
    session.reset();
    for (int i = 0; i < 2; ++i) slots[i].images = {};
  }

  void DeleteSlotGL(Slot& slot) noexcept {
    for (int i = 0; i < 2; ++i) {
      if (slot.texture[i]) gl.deleteTextures(1, &slot.texture[i]);
      if (slot.memory[i]) gl.deleteMemoryObjects(1, &slot.memory[i]);
      slot.texture[i] = slot.memory[i] = 0;
    }
  }
  void DeleteSlot(Slot& slot) noexcept {
    DeleteSlotGL(slot);
    slot.images = {};
  }

  // Never frees or waits. Safe from a noexcept path after a core Release.
  void Quarantine() noexcept {
    ambiguous = true;
    handoff = false;
    active = -1;
    if (session) session->Abandon();
    if (keep) {
      *keep = shared_from_this();
      keep.release();  // intentionally leaked: keeps this State alive forever
    }
  }

  void ImportImage(Slot& slot, unsigned i, const Device::Image& image) {
    const GLint one = 1;
    gl.createMemoryObjects(1, &slot.memory[i]);
    gl.memoryObjectParameteriv(slot.memory[i], kDedicatedMemoryExt, &one);
    gl.importMemoryWin32(slot.memory[i], image.AllocationSize(), kHandleOpaqueWin32, image.ExternalHandle());
    CheckGL("memory import");
    gl.genTextures(1, &slot.texture[i]);
    GLint previous = 0;
    gl.getIntegerv(kTextureBinding2D, &previous);
    gl.bindTexture(kTexture2D, slot.texture[i]);
    gl.texParameteri(kTexture2D, kTilingExt, static_cast<GLint>(kOptimalTiling));
    if (gl.texStorageMem2D)
      gl.texStorageMem2D(kTexture2D, 1, kR32UI, static_cast<GLsizei>(image.Width()), static_cast<GLsizei>(image.Height()),
                         slot.memory[i], 0);
    else
      gl.textureStorageMem2D(slot.texture[i], 1, kR32UI, static_cast<GLsizei>(image.Width()),
                             static_cast<GLsizei>(image.Height()), slot.memory[i], 0);
    gl.texParameteri(kTexture2D, kTextureMinFilter, static_cast<GLint>(kNearest));
    gl.texParameteri(kTexture2D, kTextureMagFilter, static_cast<GLint>(kNearest));
    gl.texParameteri(kTexture2D, kTextureWrapS, static_cast<GLint>(kClampToEdge));
    gl.texParameteri(kTexture2D, kTextureWrapT, static_cast<GLint>(kClampToEdge));
    gl.bindTexture(kTexture2D, static_cast<GLuint>(previous));
    CheckGL("texture import");
  }

  // Returns the slot holding this pair, importing it first if needed. On failure
  // the slot is cleaned (no handoff exists yet) and the exception propagates.
  int Import(const ImagePair& images) {
    for (int i = 0; i < 2; ++i)
      if (slots[i].images[0] == images[0] && slots[i].images[1] == images[1]) {
        slots[i].stamp = ++clock;
        return i;
      }
    int victim = 0;
    if (slots[0].images[0] && slots[1].images[0]) victim = slots[0].stamp <= slots[1].stamp ? 0 : 1;
    else if (slots[0].images[0]) victim = 1;
    Slot& slot = slots[victim];
    // No active handoff here: the last core Acquire proved earlier GL use complete.
    DeleteSlot(slot);
    ClearErrors();
    slot.images = images;
    try {
      for (unsigned i = 0; i < 2; ++i) ImportImage(slot, i, *images[i]);
    } catch (...) {
      DeleteSlot(slot);
      ClearErrors();
      throw;
    }
    slot.stamp = ++clock;
    return victim;
  }

  void ValidateImages(const ImagePair& images) const {
    for (const auto& image : images) {
      if (!image || !image->BelongsTo(*device) || !image->ExternalHandle() || image->AllocationSize() == 0 ||
          image->Format() != VK_FORMAT_R32_UINT || image->Width() == 0 || image->Height() == 0 ||
          image->Width() > static_cast<uint32_t>(std::numeric_limits<GLsizei>::max()) ||
          image->Height() > static_cast<uint32_t>(std::numeric_limits<GLsizei>::max()))
        throw std::runtime_error("External display: images are not external R32_UINT display images of this device");
    }
    if (images[0] == images[1]) throw std::runtime_error("External display: both screens use the same image");
  }
};

ExternalDisplay::ExternalDisplay(Context& context, std::shared_ptr<Device> device)
    : state(std::make_shared<State>(context, std::move(device))) {}

ExternalDisplay::~ExternalDisplay() {
  // An active handoff cannot be proven complete: quarantine instead of cleanup.
  if (state && state->handoff) state->Quarantine();
  state.reset();  // quarantined states survive through their own reference
}

bool ExternalDisplay::UsesDevice(const std::shared_ptr<Device>& device) const {
  return state->device == device;
}

bool ExternalDisplay::Healthy() const {
  return !state->broken && !state->ambiguous && state->session && state->session->Healthy();
}

unsigned int ExternalDisplay::Texture(unsigned screen) const {
  const State& s = *state;
  if (screen >= 2 || !s.handoff || s.active < 0 || s.ambiguous || s.broken) return 0;
  return s.slots[s.active].texture[screen];
}

void ExternalDisplay::Begin(const std::array<std::shared_ptr<Device::Image>, 2>& images) {
  State& s = *state;
  if (s.handoff) throw std::logic_error("External display: Begin during an active handoff");
  if (!Healthy()) throw std::runtime_error("External display: helper is unusable");
  s.ValidateImages(images);

  int slot;
  try {
    slot = s.Import(images);  // fallible, nothing released yet
    s.ClearErrors();
  } catch (...) {
    s.broken = true;
    throw;
  }
  try {
    s.session->Release(images);
  } catch (...) {
    s.broken = true;
    if (s.device->ExternalWorkFailed()) s.Quarantine();  // submission may have happened
    throw;
  }
  // From here the queue ownership is external: every failure is ambiguous.
  s.handoff = true;
  s.active = slot;
  try {
    const GLenum layouts[2] = {kLayoutGeneral, kLayoutGeneral};
    s.gl.waitSemaphore(s.readySemaphore, 0, nullptr, 2, s.slots[slot].texture, layouts);
    s.CheckGL("Ready wait");
  } catch (...) {
    s.Quarantine();
    throw;
  }
}

void ExternalDisplay::End() {
  State& s = *state;
  if (s.ambiguous || s.broken) throw std::runtime_error("External display: helper is unusable");
  if (!s.handoff) throw std::logic_error("External display: End without an active handoff");
  try {
    const GLenum layouts[2] = {kLayoutGeneral, kLayoutGeneral};
    s.gl.signalSemaphore(s.returnedSemaphore, 0, nullptr, 2, s.slots[s.active].texture, layouts);
    // EXT_external_objects specifies an implicit flush after this signal.
    s.CheckGL("Returned signal");  // also reports errors raised by the caller's sampling
    s.session->Acquire();
  } catch (...) {
    s.Quarantine();
    throw;
  }
  s.handoff = false;
  s.active = -1;
}
} // namespace GL
