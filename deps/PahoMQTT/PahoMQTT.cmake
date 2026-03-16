# Eclipse Paho MQTT C++ Client Library
#
# This builds both the Paho C library (required) and the Paho C++ wrapper
# The C++ library requires C++11 and provides async/sync MQTT clients

set(_paho_c_flags
  -DPAHO_BUILD_SHARED:BOOL=OFF
  -DPAHO_BUILD_STATIC:BOOL=ON
  -DPAHO_WITH_SSL:BOOL=ON
  -DPAHO_ENABLE_WEBSOCKETS:BOOL=ON
  -DPAHO_ENABLE_TESTING:BOOL=OFF
  -DPAHO_BUILD_SAMPLES:BOOL=OFF
  -DPAHO_BUILD_DOCUMENTATION:BOOL=OFF
  -DPAHO_HIGH_PERFORMANCE:BOOL=ON
)

set(_paho_cpp_flags
  -DPAHO_BUILD_SHARED:BOOL=OFF
  -DPAHO_BUILD_STATIC:BOOL=ON
  -DPAHO_WITH_SSL:BOOL=ON
  -DPAHO_ENABLE_TESTING:BOOL=OFF
  -DPAHO_BUILD_SAMPLES:BOOL=OFF
  -DPAHO_BUILD_DOCUMENTATION:BOOL=OFF
)

if (BUILD_SHARED_LIBS)
  set(_paho_static OFF)
else()
  set(_paho_static ON)
endif()

# First build Paho C library
orcaslicer_add_cmake_project(PahoMQTT_C
  URL                 https://github.com/eclipse/paho.mqtt.c/archive/refs/tags/v1.3.13.tar.gz
  DEPENDS             ${OPENSSL_PKG}
  CMAKE_ARGS
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DPAHO_BUILD_STATIC=${_paho_static}
    ${_paho_c_flags}
)

# Then build Paho C++ library (depends on C library)
orcaslicer_add_cmake_project(PahoMQTT_CPP
  URL                 https://github.com/eclipse/paho.mqtt.cpp/archive/refs/tags/v1.3.2.tar.gz
  DEPENDS             dep_PahoMQTT_C
  CMAKE_ARGS
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DPAHO_BUILD_STATIC=${_paho_static}
    -DPAHO_MQTT_C_PATH=${DESTDIR}/usr/local
    ${_paho_cpp_flags}
)

if(NOT OPENSSL_FOUND)
  add_dependencies(dep_PahoMQTT_C ${OPENSSL_PKG})
endif()

if (MSVC)
    add_debug_dep(dep_PahoMQTT_C)
    add_debug_dep(dep_PahoMQTT_CPP)
endif ()
