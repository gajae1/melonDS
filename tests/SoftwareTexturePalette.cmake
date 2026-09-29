# 557,056 shading outputs plus 3,584 outputs after palette/texture writes and
# bank unmap/remap. Golden bytes come from unmodified scalar core ae8f0e7f.
execute_process(COMMAND "${EXECUTABLE}" "${OUTPUT}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Software texture palette fixture failed: ${result}")
endif()
file(SIZE "${OUTPUT}" size)
file(SHA256 "${OUTPUT}" digest)
if(NOT size EQUAL 2242560 OR
   NOT digest STREQUAL "99cbeac2f33e97a5c6f30e712cb6bdc755b227069e7f1cb99f5c6d534cf66ce2")
    message(FATAL_ERROR "Software texture output differs from scalar baseline: ${size} bytes, ${digest}")
endif()
