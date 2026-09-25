# Emit the scale-independent optional display/native-readback compute shaders.
file(WRITE "${OUTPUT_FILE}" "// Generated; do not edit.\n#include \"EmbeddedShaders.h\"\nnamespace melonDS::Vulkan {\n")
foreach(name IN ITEMS DisplayCompose NativeReadback CaptureBlend Present.vert Present.frag)
    file(READ "${SHADER_DIR}/${name}.spv" contents HEX)
    string(REGEX REPLACE "(..)(..)(..)(..)" "0x\\4\\3\\2\\1," contents "${contents}")
    string(REPLACE "." "_" symbol "${name}")
    file(APPEND "${OUTPUT_FILE}" "std::span<const uint32_t> Embedded${symbol}() {\nstatic const uint32_t words[] = {${contents}};\nreturn words;\n}\n")
endforeach()
file(APPEND "${OUTPUT_FILE}" "}\n")
