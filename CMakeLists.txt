# vic20-recomp - VIC-20 cartridge games recompiled to native C on the vic20recomp
# toolkit.
#
#   cmake -S . -B build -G "Visual Studio 17 2022" -A x64 \
#         -DVIC20RECOMP_DIR=D:/recomp/computers/vic20recomp
#   cmake --build build --config Release
# After generating games/<game>/generated/recomp_funcs.c with m6502recomp,
# reconfigure with -D<GAME>_HAVE_RECOMP=ON to link the recompiled blocks in.
cmake_minimum_required(VERSION 3.16)
project(vic20_recomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# The toolkit. Point this at your vic20recomp checkout.
set(VIC20RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vic20recomp" CACHE PATH "vic20recomp toolkit")
if(NOT EXISTS ${VIC20RECOMP_DIR}/CMakeLists.txt)
    message(FATAL_ERROR
        "vic20recomp toolkit not found at ${VIC20RECOMP_DIR}. "
        "Pass -DVIC20RECOMP_DIR=<path> to the vic20recomp checkout.")
endif()
add_subdirectory(${VIC20RECOMP_DIR} vic20recomp_build)

add_subdirectory(games/jellymonsters)
