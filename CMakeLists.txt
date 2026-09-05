cmake_minimum_required(VERSION 3.7)
project(Filters)

include(filters.cmake)

add_definitions(-fpic)

SET(LIBDDS_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/dec_dds.c
)

SET(LIBDDS_INC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
)

add_filter(libdds
        "${LIBDDS_SRC}"
        ""
        []
        ""
        "${LIBDDS_INC}"
        ""
        "1")
