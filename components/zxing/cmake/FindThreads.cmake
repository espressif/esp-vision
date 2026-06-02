# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

if(NOT TARGET Threads::Threads)
    add_library(Threads::Threads INTERFACE IMPORTED)
endif()

set(CMAKE_THREAD_LIBS_INIT "" CACHE STRING "" FORCE)
set(CMAKE_USE_PTHREADS_INIT OFF CACHE BOOL "" FORCE)
set(Threads_FOUND TRUE CACHE BOOL "" FORCE)
set(THREADS_FOUND TRUE CACHE BOOL "" FORCE)
