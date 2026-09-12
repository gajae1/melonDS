# Optional kernels are gated by both compiler support and runtime CPU/OS state.
include(CheckCXXSourceCompiles)
option(ENABLE_SIMD "Build optional SIMD pixel/audio kernels with runtime dispatch" ON)
option(ENABLE_AVX2 "Build the AVX2 pixel and companion FMA audio kernels" ON)
option(ENABLE_AVX512 "Build AVX-512F/BW pixel kernels (benchmark before enabling)" OFF)

function(melonds_configure_pixel_kernels target)
    set(avx2 OFF)
    set(avx512 OFF)
    set(avx512bw OFF)
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
                    #if !defined(__x86_64__) && !defined(_M_X64)
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
    if (avx512)
        check_cxx_source_compiles("
            #include <immintrin.h>
            __attribute__((target(\"avx512f,avx512bw\")))
            void kernel(void* p) {
                _mm512_storeu_si512(p, _mm512_shuffle_epi8(_mm512_loadu_si512(p), _mm512_setzero_si512()));
            }
            int main() { return __builtin_cpu_supports(\"avx512f\") && __builtin_cpu_supports(\"avx512bw\"); }
        " MELONDS_HAS_AVX512BW_TARGET)
        set(avx512bw ${MELONDS_HAS_AVX512BW_TARGET})
    endif()
    target_compile_definitions(${target} PRIVATE
        MELONDS_PIXEL_AVX2=$<BOOL:${avx2}> MELONDS_PIXEL_AVX512=$<BOOL:${avx512}>
        MELONDS_PIXEL_AVX512BW=$<BOOL:${avx512bw}>
        MELONDS_PIXEL_NEON=$<BOOL:${ENABLE_SIMD}>)
    message(STATUS "${target}: AVX2=${avx2}, AVX512F=${avx512}, AVX512BW=${avx512bw}; runtime CPU/OS dispatch")
endfunction()

# The AVX2 build bundle includes its commonly available FMA companion, but
# dispatch checks FMA independently; AVX2 pixels must also work without FMA.
function(melonds_configure_audio_kernels target)
    set(sse2 OFF)
    set(fma OFF)
    if (ENABLE_SIMD)
        check_cxx_source_compiles("
            #include <immintrin.h>
            #if !defined(__x86_64__) && !defined(_M_X64)
            #error Not an x64 compiler target
            #endif
            int main() { return _mm_cvtsi128_si32(_mm_cvttpd_epi32(_mm_set1_pd(1.5))); }
        " MELONDS_HAS_AUDIO_SSE2)
        set(sse2 ${MELONDS_HAS_AUDIO_SSE2})
    endif()
    if (ENABLE_SIMD AND ENABLE_AVX2)
        check_cxx_source_compiles("
            #include <immintrin.h>
            #if !defined(__x86_64__) && !defined(_M_X64)
            #error Not an x86 compiler target
            #endif
            __attribute__((target(\"avx,fma\")))
            void kernel(double* p) {
                __m128d x = _mm_loadu_pd(p);
                _mm_storeu_pd(p, _mm_fmadd_pd(x, x, x));
            }
            int main() { return __builtin_cpu_supports(\"avx\") && __builtin_cpu_supports(\"fma\"); }
        " MELONDS_HAS_FMA_TARGET)
        set(fma ${MELONDS_HAS_FMA_TARGET})
    endif()
    target_compile_definitions(${target} PRIVATE
        MELONDS_AUDIO_SSE2=$<BOOL:${sse2}> MELONDS_AUDIO_FMA=$<BOOL:${fma}>)
    message(STATUS "${target}: audio SSE2=${sse2}, FMA=${fma}; runtime CPU/OS dispatch")
endfunction()
