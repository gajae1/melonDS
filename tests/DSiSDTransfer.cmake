# Standalone-compatible SD regression fragment. The driver only needs project()
# and enable_testing(); the parent can also include this from tests/CMakeLists.txt.
set(sd_test_root "${CMAKE_CURRENT_LIST_DIR}/..")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${sd_test_root}/src/DSi_SD.cpp")
# Compile all current SD definitions and the real header/FIFO, replacing only
# include dependencies with the test's scheduler and storage adapters.
file(READ "${sd_test_root}/src/DSi_SD.cpp" sd_source)
string(REGEX REPLACE "#include[^\n]*\n" "" sd_source "${sd_source}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/DSiSDSource.inc" "${sd_source}")
add_executable(DSiSDTransfer "${CMAKE_CURRENT_LIST_DIR}/DSiSDTransfer.cpp")
target_include_directories(DSiSDTransfer PRIVATE
    "${sd_test_root}/src" "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_features(DSiSDTransfer PRIVATE cxx_std_26)
include("${CMAKE_CURRENT_LIST_DIR}/DSiSDFATBacking.cmake")
foreach(case IN ITEMS payload-scr payload-ssr guarded-rx guarded-tx
        sector-read sector-write sd-short-read sd-short-write sd-rmw-read
        nand-short-read nand-short-write nand-seek-read nand-seek-write sd-seek-read sd-seek-write
        sd-end nand-end readonly normal16 normal32 normal-nand16 normal-nand32
        partial multiblock-read multiblock-write error-irq invalid-length recovery)
    add_test(NAME dsi-sd-${case} COMMAND DSiSDTransfer ${case})
    set_tests_properties(dsi-sd-${case} PROPERTIES TIMEOUT 10)
endforeach()
