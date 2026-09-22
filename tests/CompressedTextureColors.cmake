# The complete deterministic stream was captured from the unchanged 1.1.160
# decoder: 648 decoded cases and actual cache misses/hits, all three formats.
execute_process(COMMAND "${EXECUTABLE}" check "${OUTPUT}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Compressed texture fixture failed: ${result}")
endif()
file(SIZE "${OUTPUT}" size)
file(SHA256 "${OUTPUT}" digest)
if(NOT size EQUAL 61780472 OR
   NOT digest STREQUAL "1f5e8221d0c630d7a59e4dafd5d95b48f438ba36f8bac9a3b2e9ecb70fec4071")
    message(FATAL_ERROR "Compressed texture output differs from baseline: ${size} bytes, ${digest}")
endif()
