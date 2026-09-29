# Complete native and capture-layer output from the direct-color bitmap BG
# renderer across 2,592 fixed cases: both engines, BG2/BG3, sizes, clipping,
# window masks, capture and mosaic.
execute_process(COMMAND "${EXECUTABLE}" check "${OUTPUT}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Software bitmap BG fixture failed: ${result}")
endif()
file(SIZE "${OUTPUT}" size)
file(SHA256 "${OUTPUT}" digest)
if(NOT size EQUAL 18579456 OR
   NOT digest STREQUAL "77c78d74a5d81df11d44bd52ea0ea3f93f81bfdb3f23e50ef29152cab3db1729")
    message(FATAL_ERROR "Software bitmap BG output differs from baseline: ${size} bytes, ${digest}")
endif()
