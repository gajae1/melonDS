# Complete output from the unchanged 1.1.161 software renderer across 6,144
# fixed-seed text-BG cases, including both engines, palettes, flips and mosaic.
execute_process(COMMAND "${EXECUTABLE}" check "${OUTPUT}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Software text BG fixture failed: ${result}")
endif()
file(SIZE "${OUTPUT}" size)
file(SHA256 "${OUTPUT}" digest)
if(NOT size EQUAL 18874372 OR
   NOT digest STREQUAL "211d4269d23e72d8e4f81114e0c176b19e0d68bd0290bfcc0d11b09cc49e4941")
    message(FATAL_ERROR "Software text BG output differs from baseline: ${size} bytes, ${digest}")
endif()
