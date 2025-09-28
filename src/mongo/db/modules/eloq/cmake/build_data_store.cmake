message(STATUS "WITH_DATA_STORE: ${WITH_DATA_STORE}") # WITH_DATA_STORE is a global cache variable

set(LOCAL_DATA_STORE_LIBRARY "") # Initialize
set(LOCAL_DATA_STORE_INCLUDE_DIRS "")

if(WITH_DATA_STORE STREQUAL "DYNAMODB")
    set(KV_STORAGE_VAL 1 CACHE STRING "dynamodb" FORCE)
    add_compile_definitions(DATA_STORE_TYPE_DYNAMODB)
    message(STATUS "DataStore: Configured for DYNAMODB. KV_STORAGE_VAL=1. Added DATA_STORE_TYPE_DYNAMODB definition.")
    find_package(AWSSDK REQUIRED COMPONENTS dynamodb) # Specific to this data store type
    set(LOCAL_DATA_STORE_LIBRARY ${AWSSDK_LIBRARIES})
    set(LOCAL_DATA_STORE_INCLUDE_DIRS ${AWSSDK_INCLUDE_DIRS})
    message(STATUS "DataStore: Found AWSSDK for DynamoDB: ${AWSSDK_LIBRARIES} (Includes: ${AWSSDK_INCLUDE_DIRS})")
elseif(WITH_DATA_STORE STREQUAL "BIGTABLE")
    set(KV_STORAGE_VAL 2 CACHE STRING "big table" FORCE)
    add_compile_definitions(DATA_STORE_TYPE_BIGTABLE)
    message(STATUS "DataStore: Configured for BIGTABLE. KV_STORAGE_VAL=2. Added DATA_STORE_TYPE_BIGTABLE definition.")
    find_package(google_cloud_cpp_bigtable REQUIRED) # Specific to this data store type
    set(LOCAL_DATA_STORE_LIBRARY google-cloud-cpp::bigtable) # Imported target
    message(STATUS "DataStore: Found google-cloud-cpp-bigtable: ${LOCAL_DATA_STORE_LIBRARY}")
    # For imported targets, include directories are usually handled automatically.
    # set(LOCAL_DATA_STORE_INCLUDE_DIRS ${google_cloud_cpp_bigtable_INCLUDE_DIRS}) # If needed
elseif(WITH_DATA_STORE STREQUAL "ELOQDSS_ROCKSDB_CLOUD_S3" OR WITH_DATA_STORE STREQUAL "ELOQDSS_ROCKSDB_CLOUD_GCS")
    if(WITH_DATA_STORE STREQUAL "ELOQDSS_ROCKSDB_CLOUD_S3")
        set(KV_STORAGE_VAL 3 CACHE STRING "eloq_ds_s3" FORCE) # More specific
        add_compile_definitions(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_S3)
        add_compile_definitions(ROCKSDB_CLOUD_FS_TYPE=1) # Module specific definition
        message(STATUS "DataStore: Configured for ELOQDSS_ROCKSDB_CLOUD_S3. KV_STORAGE_VAL=3. Added definitions DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_S3, ROCKSDB_CLOUD_FS_TYPE=1.")
    elseif(WITH_DATA_STORE STREQUAL "ELOQDSS_ROCKSDB_CLOUD_GCS")
        set(KV_STORAGE_VAL 3 CACHE STRING "eloq_ds_gcs" FORCE) # More specific
        add_compile_definitions(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_GCS)
        add_compile_definitions(ROCKSDB_CLOUD_FS_TYPE=2) # Module specific definition
        message(STATUS "DataStore: Configured for ELOQDSS_ROCKSDB_CLOUD_GCS. KV_STORAGE_VAL=3. Added definitions DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_GCS, ROCKSDB_CLOUD_FS_TYPE=2.")
    endif()

    if(NOT ROCKSDB_FOUND)
        message(FATAL_ERROR "Data store ${WITH_DATA_STORE} requires RocksDB, but it was not found by find_dependencies.cmake.")
    endif()
    # ROCKSDB_GLOBAL_LIBRARIES and ROCKSDB_GLOBAL_INCLUDE_DIRS are set by find_dependencies.cmake
    set(LOCAL_DATA_STORE_LIBRARY ${ROCKSDB_GLOBAL_LIBRARIES})
    set(LOCAL_DATA_STORE_INCLUDE_DIRS ${ROCKSDB_GLOBAL_INCLUDE_DIRS}) # These are already global includes

    # Proto compilation for ELOQDSS
    set(ELOQ_DSS_PROTO_DIR_PATH ${CMAKE_CURRENT_SOURCE_DIR}/store_handler/eloq_data_store_service)
    message(NOTICE "data store service proto dir: ${ELOQ_DSS_PROTO_DIR_PATH}")
    compile_protos_in_directory(${ELOQ_DSS_PROTO_DIR_PATH})
    set(DS_COMPILED_PROTO_FILES ${COMPILED_PROTO_CC_FILES})
    message(STATUS "DataStore: ELOQDSS compiled protos: ${DS_COMPILED_PROTO_FILES}")
    list(APPEND LOCAL_DATA_STORE_INCLUDE_DIRS ${ELOQ_DSS_PROTO_DIR_PATH}) # Add proto dir to includes for this target
elseif(WITH_DATA_STORE STREQUAL "ELOQDSS_ROCKSDB")
    set(KV_STORAGE_VAL 3 CACHE STRING "eloq_ds" FORCE)
    add_compile_definitions(DATA_STORE_TYPE_ELOQDSS_ROCKSDB)
    # Proto compilation for ELOQDSS
    set(ELOQ_DSS_PROTO_DIR_PATH ${CMAKE_CURRENT_SOURCE_DIR}/store_handler/eloq_data_store_service)
    message(NOTICE "data store service proto dir: ${ELOQ_DSS_PROTO_DIR_PATH}")
    compile_protos_in_directory(${ELOQ_DSS_PROTO_DIR_PATH})
    set(DS_COMPILED_PROTO_FILES ${COMPILED_PROTO_CC_FILES})
    message(STATUS "DataStore: ELOQDSS compiled protos: ${DS_COMPILED_PROTO_FILES}")
    list(APPEND LOCAL_DATA_STORE_INCLUDE_DIRS ${ELOQ_DSS_PROTO_DIR_PATH}) # Add proto dir to includes for this target
elseif(WITH_DATA_STORE STREQUAL "ELOQDSS_ELOQSTORE")
    set(KV_STORAGE_VAL 4 CACHE STRING "eloq_ds_eloqstore" FORCE)
    add_compile_definitions(DATA_STORE_TYPE_ELOQDSS_ELOQSTORE)
    message(STATUS "DataStore: Configured for ELOQDSS_ELOQSTORE.")
    # Proto compilation for ELOQDSS
    set(ELOQ_DSS_PROTO_DIR_PATH ${CMAKE_CURRENT_SOURCE_DIR}/store_handler/eloq_data_store_service)
    message(NOTICE "data store service proto dir: ${ELOQ_DSS_PROTO_DIR_PATH}")
    compile_protos_in_directory(${ELOQ_DSS_PROTO_DIR_PATH})
    set(DS_COMPILED_PROTO_FILES ${COMPILED_PROTO_CC_FILES})
    message(STATUS "DataStore: ELOQDSS compiled protos: ${DS_COMPILED_PROTO_FILES}")
    list(APPEND LOCAL_DATA_STORE_INCLUDE_DIRS ${ELOQ_DSS_PROTO_DIR_PATH}) # Add proto dir to includes for this target

    option(WITH_TXSERVICE "Whether compile eloqstore with txservice" ON)
    set(ELOQSTORE_PARENT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/store_handler/eloq_data_store_service CACHE PATH "EloqStore parent directory")
    INCLUDE(${CMAKE_CURRENT_SOURCE_DIR}/store_handler/eloq_data_store_service/build_eloq_store.cmake)

    list(APPEND LOCAL_DATA_STORE_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/store_handler/eloq_data_store_service/eloqstore)
    set(ELOQSTORE_LIBRARY eloqstore)
    set(LOCAL_DATA_STORE_LIBRARY ${ELOQSTORE_LIBRARY})
else()
    message(FATAL_ERROR "Unset or unsupported WITH_DATA_STORE: ${WITH_DATA_STORE}")
endif()

# RocksDB Cloud SDK finding logic is now in find_dependencies.cmake.
# The ROCKSDB_INCLUDE_PATH and ROCKSDB_LIBRARIES variables used below
# should now be ROCKSDB_GLOBAL_INCLUDE_DIRS and ROCKSDB_GLOBAL_LIBRARIES from find_dependencies.cmake.

set(DATA_STORE_BASE_INCLUDE_DIR store_handler) # Base include for all data store handlers
message(STATUS "DataStore: Base include directory for handlers: ${DATA_STORE_BASE_INCLUDE_DIR}")
list(APPEND LOCAL_DATA_STORE_INCLUDE_DIRS ${DATA_STORE_BASE_INCLUDE_DIR} ${Protobuf_INCLUDE_DIRS})
message(STATUS "DataStore: Appended common includes (store_handler, mimalloc, protobuf) to LOCAL_DATA_STORE_INCLUDE_DIRS.")


    # LOCAL_DATA_STORE_LIBRARY already set
if(WITH_DATA_STORE STREQUAL "DYNAMODB")
    set(DATA_STORE_SOURCES ${DATA_STORE_SOURCES}
        store_handler/dynamo_handler.cpp
        store_handler/dynamo_scanner.cpp)
    # LOCAL_DATA_STORE_LIBRARY already set
elseif(WITH_DATA_STORE STREQUAL "BIGTABLE")
    set(DATA_STORE_SOURCES
        ${DATA_STORE_SOURCES}
        store_handler/bigtable_handler.cpp
        store_handler/bigtable_scanner.cpp)
    # LOCAL_DATA_STORE_LIBRARY already set
elseif(WITH_DATA_STORE STREQUAL "ELOQDSS_ROCKSDB_CLOUD_S3" OR WITH_DATA_STORE STREQUAL "ELOQDSS_ROCKSDB_CLOUD_GCS" OR WITH_DATA_STORE STREQUAL "ELOQDSS_ROCKSDB")
    set(_ELOQDSS_SOURCES_LIST
        store_handler/data_store_service_client.cpp
        store_handler/data_store_service_client_closure.cpp
        store_handler/data_store_service_scanner.cpp
        store_handler/store_util.cpp
        store_handler/eloq_data_store_service/thread_worker_pool.cpp
        store_handler/eloq_data_store_service/data_store_service.cpp
        store_handler/eloq_data_store_service/data_store_fault_inject.cpp
        store_handler/eloq_data_store_service/data_store_service_config.cpp
        # ds_request.pb.cc will be added from DS_COMPILED_PROTO_FILES
        store_handler/eloq_data_store_service/rocksdb_config.cpp
        store_handler/eloq_data_store_service/rocksdb_data_store_common.cpp
    )

    if (WITH_DATA_STORE STREQUAL "ELOQDSS_ROCKSDB_CLOUD_S3" OR WITH_DATA_STORE STREQUAL "ELOQDSS_ROCKSDB_CLOUD_GCS")
      SET(_ELOQDSS_SOURCES_LIST
        ${_ELOQDSS_SOURCES_LIST}
          store_handler/eloq_data_store_service/rocksdb_cloud_data_store.cpp)
    elseif (WITH_DATA_STORE STREQUAL "ELOQDSS_ROCKSDB")
      SET(_ELOQDSS_SOURCES_LIST
        ${_ELOQDSS_SOURCES_LIST}
          store_handler/eloq_data_store_service/rocksdb_data_store.cpp)
    endif()

    if(DS_COMPILED_PROTO_FILES)
        list(APPEND _ELOQDSS_SOURCES_LIST ${DS_COMPILED_PROTO_FILES})
        message(STATUS "DataStore: Appended DS_COMPILED_PROTO_FILES to _ELOQDSS_SOURCES_LIST.")
    endif()
    set(DATA_STORE_SOURCES ${_ELOQDSS_SOURCES_LIST})
    # LOCAL_DATA_STORE_LIBRARY (ROCKSDB_GLOBAL_LIBRARIES) already set
elseif(WITH_DATA_STORE STREQUAL "ELOQDSS_ELOQSTORE")
    set(_ELOQDSS_SOURCES_LIST
        store_handler/data_store_service_client.cpp
        store_handler/data_store_service_client_closure.cpp
        store_handler/data_store_service_scanner.cpp
        store_handler/store_util.cpp
        store_handler/eloq_data_store_service/thread_worker_pool.cpp
        store_handler/eloq_data_store_service/data_store_service.cpp
        store_handler/eloq_data_store_service/data_store_fault_inject.cpp
        store_handler/eloq_data_store_service/data_store_service_config.cpp
        store_handler/eloq_data_store_service/eloq_store_data_store.cpp)

    if(DS_COMPILED_PROTO_FILES)
        list(APPEND _ELOQDSS_SOURCES_LIST ${DS_COMPILED_PROTO_FILES})
        message(STATUS "DataStore: Appended DS_COMPILED_PROTO_FILES to _ELOQDSS_SOURCES_LIST.")
    endif()
    set(DATA_STORE_SOURCES ${_ELOQDSS_SOURCES_LIST})
else()
    message(FATAL_ERROR "Unset or unsupported WITH_DATA_STORE for sources: ${WITH_DATA_STORE}")
endif()

message(STATUS "DATA_STORE_SOURCES: ${DATA_STORE_SOURCES}")
message(STATUS "DATA_STORE_LIBRARY (effective for linking): ${LOCAL_DATA_STORE_LIBRARY}")
message(STATUS "DATA_STORE_INCLUDE_DIRS (effective for compilation): ${LOCAL_DATA_STORE_INCLUDE_DIRS}")


# Create object library for data store sources
add_library(DATA_STORE_SERVICE_OBJ OBJECT ${DATA_STORE_SOURCES})
target_include_directories(DATA_STORE_SERVICE_OBJ PUBLIC ${LOCAL_DATA_STORE_INCLUDE_DIRS})

# Create shared library from object library
add_library(datastore_shared SHARED $<TARGET_OBJECTS:DATA_STORE_SERVICE_OBJ>)

set(DATA_STORE_LINKER_FLAGS "")
# Add explicit dependency to ensure eloqstore_static is built before datastore_shared
if(WITH_DATA_STORE STREQUAL "ELOQDSS_ELOQSTORE")
    add_dependencies(datastore_shared ${ELOQSTORE_LIBRARY})
    # Add --whole-archive flag to link eloqstore static library, if no this flag, not all the symbols in eloqstore will be linked.
    set(DATA_STORE_LINKER_FLAGS "-Wl,--whole-archive")
endif()

target_link_libraries(datastore_shared PUBLIC ${PROTOBUF_LIBRARIES} ${DATA_STORE_LINKER_FLAGS} ${LOCAL_DATA_STORE_LIBRARY} -Wl,--no-whole-archive) # Add PROTOBUF_LIBRARIES if protos are used

set_target_properties(datastore_shared PROPERTIES OUTPUT_NAME datastore)
set_target_properties(datastore_shared PROPERTIES INSTALL_RPATH "$ORIGIN")
# ... (message logging for shared lib)

if((WITH_DATA_STORE STREQUAL "ELOQDSS_ROCKSDB_CLOUD_S3")
    OR (WITH_DATA_STORE STREQUAL "ELOQDSS_ROCKSDB_CLOUD_GCS"))
    add_subdirectory(store_handler/eloq_data_store_service)
endif()
