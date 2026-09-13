# Binary resources are compiled into the core; no runtime path or file I/O.
file(READ "${INPUT_FILE}" CONTENT HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," CONTENT "${CONTENT}")
file(WRITE "${OUTPUT_FILE}"
    "#include <cstddef>\nnamespace melonDS {\nextern const unsigned char ${VAR_NAME}[] = {${CONTENT}};\nextern const std::size_t ${VAR_NAME}Size = sizeof(${VAR_NAME});\n}\n")
