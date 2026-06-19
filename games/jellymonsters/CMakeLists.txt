# Jelly Monsters host: the runner/recompiled driver, linked against the
# vic20recomp runtime + the recompiler's decoder. The recompiled game C
# (generated/) is linked in when JELLYMONSTERS_HAVE_RECOMP=ON.
set(TC ${VIC20RECOMP_DIR}/tools/m6502recomp)   # CLI-side 6502 decoder

option(JELLYMONSTERS_HAVE_RECOMP "Link the generated recompiled C" OFF)

add_executable(jellymonsters
    src/host.c
    ${TC}/decode.c
)
target_include_directories(jellymonsters PRIVATE
    ${TC}
    ${VIC20RECOMP_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/generated
)
target_link_libraries(jellymonsters PRIVATE vic20recomp)

if(JELLYMONSTERS_HAVE_RECOMP)
    target_sources(jellymonsters PRIVATE generated/recomp_funcs.c)
    target_compile_definitions(jellymonsters PRIVATE JELLYMONSTERS_HAVE_RECOMP=1)
    message(STATUS "jellymonsters: recompiled image linked (hybrid build)")
else()
    message(STATUS "jellymonsters: no recompiled image - interpreter runner")
endif()
set_target_properties(jellymonsters PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
