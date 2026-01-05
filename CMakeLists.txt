cmake_minimum_required(VERSION 3.16)
project(sparta_mon C)

set(CMAKE_C_STANDARD 11)

add_executable(sparta-mon sparta_mon.c)
target_compile_definitions(sparta-mon PRIVATE _GNU_SOURCE)

if (WIN32)
  # Build PDCurses (WinCon backend)
  file(GLOB PDC_CORE "${CMAKE_SOURCE_DIR}/deps/PDCurses/pdcurses/*.c")
  file(GLOB PDC_WIN  "${CMAKE_SOURCE_DIR}/deps/PDCurses/wincon/*.c")

  add_library(pdcurses STATIC ${PDC_CORE} ${PDC_WIN})
  target_include_directories(pdcurses PUBLIC
    "${CMAKE_SOURCE_DIR}/deps/PDCurses"
    "${CMAKE_SOURCE_DIR}/deps/PDCurses/pdcurses"
    "${CMAKE_SOURCE_DIR}/deps/PDCurses/wincon"
  )

  target_link_libraries(sparta-mon PRIVATE pdcurses user32)

else()
  find_package(Curses REQUIRED)
  target_include_directories(sparta-mon PRIVATE ${CURSES_INCLUDE_DIR})
  target_link_libraries(sparta-mon PRIVATE ${CURSES_LIBRARIES})

endif()
