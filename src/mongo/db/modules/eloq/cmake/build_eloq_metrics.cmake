set(ELOQ_METRICS_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/eloq_metrics)
set(ELOQ_METRICS_SRC_DIR "${ELOQ_METRICS_ROOT_DIR}/src")
set(ELOQ_METRICS_INCLUDE_DIR "${ELOQ_METRICS_ROOT_DIR}/include")

option(ENABLE_BENCHMARK "Whether enable google benchmark" OFF)
option(WITH_GLOG "Whether use glog" OFF)
option(BRPC_WITH_GLOG "brpc with glog" ON)
option(ENABLE_ELOQ_METRICS_APP "Whether enable ELOQ metrics app" OFF)

message(STATUS "ENABLE_BENCHMARK ${ENABLE_BENCHMARK}")
message(STATUS "WITH_GLOG ${WITH_GLOG}")
message(STATUS "BRPC WITH_GLOG ${WITH_GLOG}")
message(STATUS "ENABLE_ELOQ_METRICS_APP ${ENABLE_ELOQ_METRICS_APP}")
message(STATUS "ELOQ_METRICS_ROOT_DIR ${ELOQ_METRICS_ROOT_DIR}")

if(POLICY CMP0074)
    cmake_policy(SET CMP0074 NEW)
endif()

if(POLICY CMP0054)
    cmake_policy(SET CMP0054 NEW)
endif()

find_package(prometheus-cpp CONFIG REQUIRED)

set(ELOQ_METRICS_TARGET_SOURCE_LIST
    ${ELOQ_METRICS_INCLUDE_DIR}/metrics.h
    ${ELOQ_METRICS_INCLUDE_DIR}/meter.h
    ${ELOQ_METRICS_INCLUDE_DIR}/metrics_collector.h
    ${ELOQ_METRICS_INCLUDE_DIR}/prometheus_collector.h
    ${ELOQ_METRICS_INCLUDE_DIR}/metrics_manager.h
    ${ELOQ_METRICS_SRC_DIR}/metrics.cc
    ${ELOQ_METRICS_SRC_DIR}/prometheus_collector.cc
    ${ELOQ_METRICS_SRC_DIR}/metrics_manager.cc
)

add_library(ELOQ_METRICS_OBJ OBJECT ${ELOQ_METRICS_TARGET_SOURCE_LIST})
target_include_directories(ELOQ_METRICS_OBJ PUBLIC ${ELOQ_METRICS_INCLUDE_DIR})

add_library(eloq_metrics_static STATIC $<TARGET_OBJECTS:ELOQ_METRICS_OBJ>)
target_link_libraries(eloq_metrics_static PUBLIC prometheus-cpp::pull)
set_target_properties(eloq_metrics_static PROPERTIES OUTPUT_NAME eloq_metrics)

add_library(eloq_metrics_shared SHARED $<TARGET_OBJECTS:ELOQ_METRICS_OBJ>)
target_link_libraries(eloq_metrics_shared PUBLIC prometheus-cpp::pull)
set_target_properties(eloq_metrics_shared PROPERTIES OUTPUT_NAME eloq_metrics)