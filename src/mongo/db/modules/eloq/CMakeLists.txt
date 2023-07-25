cmake_minimum_required(VERSION 3.8)
project(eloq)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-parentheses -Wno-error -W -fPIC -fno-omit-frame-pointer")

option(EXT_TX_PROC_ENABLED "Allows external threads to move forward the tx service." ON)
message(NOTICE "EXT_TX_PROC_ENABLED : ${EXT_TX_PROC_ENABLED}")

if(EXT_TX_PROC_ENABLED)
    add_definitions(-DEXT_TX_PROC_ENABLED)
endif()

option(FORK_HM_PROCESS "Whether fork host manager process" OFF)
message(NOTICE "FORK_HM_PROCESS : ${FORK_HM_PROCESS}")

if(FORK_HM_PROCESS)
    add_definitions(-DFORK_HM_PROCESS)
endif()

option(RANGE_PARTITION_ENABLED "Whether enable range partition" ON)
message(NOTICE "RANGE_PARTITION_ENABLED : ${RANGE_PARTITION_ENABLED}")

option(METRICS_COLLECTOR_ENABLE "Whether enable metrics collector" OFF)
message(NOTICE "METRICS_COLLECTOR_ENABLE : ${METRICS_COLLECTOR_ENABLE}")

option(SMALL_RANGE "Whether enable small range" OFF)
message(NOTICE "SMALL_RANGE : ${SMALL_RANGE}")

option(USE_ASAN "USE ASAN" OFF)
message(NOTICE "USE_ASAN: ${USE_ASAN}")

if(USE_ASAN)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address")
endif()

include(cmake/build_cass_driver.cmake)
include(cmake/build_eloq_metrics.cmake)
include(cmake/build_tx_service.cmake)
include(cmake/build_log_service.cmake)


include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/tx_service/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/tx_service/include/cc)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/tx_service/include/remote)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/tx_service/include/fault)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/tx_service/include/store)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/tx_service/tx-log-protos)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/eloq_metrics/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/log_service/include)

option(USE_GPERFTOOLS OFF)
message(NOTICE "USE_GPERFTOOLS: ${USE_GPERFTOOLS}")

if(USE_GPERFTOOLS)
    find_path(GPERFTOOLS_INCLUDE_DIR NAMES gperftools/heap-profiler.h)
    find_library(GPERFTOOLS_LIBRARIES NAMES tcmalloc_and_profiler)
    include_directories(${GPERFTOOLS_INCLUDE_DIR})
    link_libraries(${GPERFTOOLS_LIBRARIES})
    add_compile_definitions(BRPC_ENABLE_CPU_PROFILER)
endif()



# install tx_service lib and includes files
install(TARGETS txservice_shared
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib)

install(TARGETS logservice_shared
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib)

install(TARGETS eloq_metrics_shared
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib)
if(FORK_HM_PROCESS)
    install(TARGETS host_manager 
            RUNTIME DESTINATION bin)
endif()        