CMAKE_MINIMUM_REQUIRED(VERSION 2.8)

PROJECT(COCAINE-CORE)

SET(COCAINE_VERSION_MAJOR   0)
SET(COCAINE_VERSION_MINOR   12)
SET(COCAINE_VERSION_RELEASE 5)

SET(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

IF(APPLE)
    SET(CMAKE_MACOSX_RPATH ON)
ENDIF()

OPTION(COCAINE_ALLOW_TESTS "Build Tests" OFF)
OPTION(COCAINE_ALLOW_BENCHMARKS "Build Benchmarking Tools" OFF)
OPTION(COCAINE_DEBUG OFF)

# Import our CMake modules.
INCLUDE(cmake/locate_library.cmake)
INCLUDE(foreign/modules/AddCXXCompilerFlag.cmake)

IF(NOT APPLE)
    LOCATE_LIBRARY(LIBBFD "bfd.h" "bfd")
    SET(LIBBFD_LIBRARY "bfd" "dl")
ENDIF()

FIND_PACKAGE(Boost 1.46.0 REQUIRED COMPONENTS
    filesystem
    program_options
    system
    thread)

LOCATE_LIBRARY(LIBLTDL "ltdl.h" "ltdl")
LOCATE_LIBRARY(LIBMSGPACK "msgpack.hpp" "msgpack")
LOCATE_LIBRARY(LIBMHASH "mhash.h" "mhash")

IF(NOT APPLE)
    LOCATE_LIBRARY(LIBUUID "uuid/uuid.h" "uuid")
    SET(LIBUUID_LIBRARY "uuid")
ENDIF()

CONFIGURE_FILE(
    "${PROJECT_SOURCE_DIR}/config.hpp.in"
    "${PROJECT_SOURCE_DIR}/include/cocaine/config.hpp")

INCLUDE_DIRECTORIES(SYSTEM
    ${Boost_INCLUDE_DIRS}
    ${LIBMHASH_INCLUDE_DIRS}
    ${LIBMSGPACK_INCLUDE_DIRS}
    ${LIBLTDL_INCLUDE_DIRS}
    # Bundled third-party libraries.
    ${PROJECT_SOURCE_DIR}/foreign/asio/asio/include
    ${PROJECT_SOURCE_DIR}/foreign/backward-cpp
    ${PROJECT_SOURCE_DIR}/foreign/rapidjson/include)

INCLUDE_DIRECTORIES(BEFORE
    ${PROJECT_SOURCE_DIR}/include)

LINK_DIRECTORIES(
    ${Boost_LIBRARY_DIRS}
    ${LIBMHASH_LIBRARY_DIRS}
    ${LIBMSGPACK_LIBRARY_DIRS}
    ${LIBLTDL_LIBRARY_DIRS})

ADD_LIBRARY(cocaine-core SHARED
    src/actor.cpp
    src/actor_unix.cpp
    src/api.cpp
    src/chamber.cpp
    src/cluster/multicast.cpp
    src/cluster/predefine.cpp
    src/context.cpp
    src/context/config.cpp
    src/context/mapper.cpp
    src/crypto.cpp
    src/defaults.cpp
    src/dispatch.cpp
    src/dynamic.cpp
    src/engine.cpp
    src/errors.cpp
    src/essentials.cpp
    src/gateway/adhoc.cpp
    src/header.cpp
    src/logging.cpp
    src/repository.cpp
    src/service/locator.cpp
    src/service/locator/routing.cpp
    src/service/logging.cpp
    src/service/storage.cpp
    src/session.cpp
    src/storage/files.cpp
    src/trace.cpp
    src/trace/logger.cpp
    src/unique_id.cpp)

TARGET_LINK_LIBRARIES(cocaine-core
    ${Boost_LIBRARIES}
    ltdl
    mhash
    msgpack
    blackhole
    ${LIBUUID_LIBRARY})

SET_TARGET_PROPERTIES(cocaine-core PROPERTIES
    VERSION 3)

ADD_EXECUTABLE(cocaine-runtime
    src/runtime/logging.cpp
    src/runtime/pid_file.cpp
    src/runtime/runtime.cpp)

TARGET_LINK_LIBRARIES(cocaine-runtime
    stdc++
    ${LIBBFD_LIBRARY}
    ${Boost_LIBRARIES}
    blackhole
    cocaine-core)

# Try and enable C++11.
ADD_CXX_COMPILER_FLAG(-std=c++11)

IF(NOT HAVE_CXX_FLAG_STD_CXX11)
  ADD_CXX_COMPILER_FLAG(-std=c++0x)
ENDIF()

# Turn compiler warnings.
ADD_CXX_COMPILER_FLAG(-Wall)
ADD_CXX_COMPILER_FLAG(-Wextra)
ADD_CXX_COMPILER_FLAG(-Werror RELEASE)
ADD_CXX_COMPILER_FLAG(-Woverloaded-virtual)
ADD_CXX_COMPILER_FLAG(-pedantic)
ADD_CXX_COMPILER_FLAG(-pedantic-errors)

IF(COCAINE_ALLOW_TESTS OR COCAINE_ALLOW_BENCHMARKS)
    ADD_CUSTOM_TARGET(test
        COMMAND ${CMAKE_BINARY_DIR}/tests/cocaine-core-unit
        DEPENDS cocaine-core-unit
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
    ADD_SUBDIRECTORY(tests)
ENDIF()

IF(NOT COCAINE_LIBDIR)
    SET(COCAINE_LIBDIR lib)
ENDIF()

INSTALL(
    TARGETS
        cocaine-core
        cocaine-runtime
    RUNTIME DESTINATION bin COMPONENT runtime
    LIBRARY DESTINATION ${COCAINE_LIBDIR} COMPONENT runtime
    ARCHIVE DESTINATION ${COCAINE_LIBDIR} COMPONENT development)

INSTALL(
    DIRECTORY
        include/
    DESTINATION include
    COMPONENT development
    PATTERN "detail" EXCLUDE)
