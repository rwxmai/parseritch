# AF_XDP receiver: libbpf + libxdp, plus the BPF filter program compiled
# with clang's BPF target. Included from the top-level CMakeLists.txt when
# ITCH_ENABLE_XDP=ON (Linux only).

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "ITCH_ENABLE_XDP requires Linux")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBXDP REQUIRED IMPORTED_TARGET libxdp)
pkg_check_modules(LIBBPF REQUIRED IMPORTED_TARGET libbpf)
find_program(ITCH_BPF_CLANG NAMES clang clang-18 clang-19 clang-17 REQUIRED)

set(ITCH_BPF_OBJECT ${CMAKE_BINARY_DIR}/xdp_udp_filter.bpf.o)
add_custom_command(
    OUTPUT ${ITCH_BPF_OBJECT}
    COMMAND ${ITCH_BPF_CLANG} -O2 -g -target bpf -Wall -Werror
            # <asm/types.h> lives in the multiarch include dir on Debian/Ubuntu
            -I/usr/include/${CMAKE_LIBRARY_ARCHITECTURE}
            -c ${CMAKE_SOURCE_DIR}/bpf/xdp_udp_filter.bpf.c -o ${ITCH_BPF_OBJECT}
    DEPENDS ${CMAKE_SOURCE_DIR}/bpf/xdp_udp_filter.bpf.c
    COMMENT "Compiling BPF program xdp_udp_filter.bpf.o"
    VERBATIM)
add_custom_target(itch_bpf ALL DEPENDS ${ITCH_BPF_OBJECT})

# One receiver library per ISA variant, like everything else.
foreach(suffix "" "_avx2")
    if(TARGET itch${suffix})
        add_library(itch_xdp${suffix} STATIC src/net/xdp_receiver.cpp)
        target_link_libraries(itch_xdp${suffix} PUBLIC itch${suffix} PkgConfig::LIBXDP PkgConfig::LIBBPF)
        target_link_libraries(feed_handler${suffix} PRIVATE itch_xdp${suffix})
        target_compile_definitions(feed_handler${suffix} PRIVATE
            ITCH_HAVE_XDP ITCH_XDP_BPF_OBJECT="${ITCH_BPF_OBJECT}")
        add_dependencies(feed_handler${suffix} itch_bpf)
    endif()
endforeach()
