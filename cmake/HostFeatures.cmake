# Optional kernels are gated by both compiler support and runtime CPU/OS state.
include(CheckCXXSourceCompiles)
option(ENABLE_SIMD "Build optional SIMD pixel kernels with runtime dispatch" ON)
option(ENABLE_AVX2 "Build optional AVX2 pixel conversion" ON)
option(ENABLE_AVX512 "Build optional AVX-512F conversion (benchmark before enabling)" OFF)

function(melonds_configure_pixel_kernels target)
    set(avx2 OFF)
    set(avx512 OFF)
    if (ENABLE_SIMD)
        foreach(isa IN ITEMS AVX2 AVX512)
            if (ENABLE_${isa})
                if (isa STREQUAL "AVX2")
                    set(feature "avx2")
                    set(operation "_mm256_storeu_si256((__m256i*)p, _mm256_set1_epi32(1))")
                else()
                    set(feature "avx512f")
                    set(operation "_mm512_storeu_si512(p, _mm512_set1_epi32(1))")
                endif()
                check_cxx_source_compiles("
                    #include <immintrin.h>
                    #if !defined(__x86_64__) && !defined(__i386__)
                    #error Not an x86 compiler target
                    #endif
                    __attribute__((target(\"${feature}\")))
                    void kernel(int* p) { ${operation}; }
                    int main() { return __builtin_cpu_supports(\"${feature}\"); }
                " MELONDS_HAS_${isa}_TARGET)
                if (MELONDS_HAS_${isa}_TARGET)
                    if (isa STREQUAL "AVX2")
                        set(avx2 ON)
                    else()
                        set(avx512 ON)
                    endif()
                endif()
            endif()
        endforeach()
    endif()
    target_compile_definitions(${target} PRIVATE
        MELONDS_PIXEL_AVX2=$<BOOL:${avx2}> MELONDS_PIXEL_AVX512=$<BOOL:${avx512}>)
    message(STATUS "${target}: AVX2=${avx2}, AVX512F=${avx512}; runtime CPU/OS dispatch")
endfunction()
