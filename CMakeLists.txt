cmake_minimum_required (VERSION 2.8)
project(psclient)

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin)
set(LIBRARY_OUTPUT_PATH ${PROJECT_BINARY_DIR}/lib)
set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake" ${CMAKE_MODULE_PATH})
set(CMAKE_LIBRARY_PATH ${CMAKE_LIBRARY_PATH} /usr/local/lib)
set(CMAKE_LIBRARY_PATH ${CMAKE_LIBRARY_PATH} /usr/lib)
message("CMAKE_LIBRARY_PATH: ${CMAKE_LIBRARY_PATH}")
set(CMAKE_INSTALL_PREFIX /usr/local)

set(CMAKE_C_FLAGS ${CMAKE_C_FLAGS} "-g -Wall")
message("CMAKE_C_FLAGS: ${CMAKE_C_FLAGS}")
set(SOFIASIP_LIBRARIES "/usr/lib/libsofia-sip-ua.so")
set(GOBJECT_LIBRARIES "/usr/local/lib/libgobject-2.0.so")
set(GTHREAD_LIBRARIES "/usr/local/lib/libgthread-2.0.so")
set(GIO_LIBRARIES "/usr/local/lib/libgio-2.0.so")
set(SOFIASIP_INCLUDE_DIR "/usr/include/sofia-sip-1.12/")
set(SRTP_LIBRARIES "/usr/local/lib/libsrtp.so")
set(SRTP_INCLUDE_DIR "/usr/local/include")

add_definitions(-DSTDC_HEADERS=1)
add_definitions(-DHAVE_SYS_TYPES_H=1)
add_definitions(-DHAVE_SYS_STAT_H=1)
add_definitions(-DHAVE_STDLIB_H=1)
add_definitions(-DHAVE_STRING_H=1)
add_definitions(-DHAVE_MEMORY_H=1)
add_definitions(-DHAVE_STRINGS_H=1)
add_definitions(-DHAVE_INTTYPES_H=1)
add_definitions(-DHAVE_STDINT_H=1)
add_definitions(-DHAVE_UNISTD_H=1)

add_definitions(-D_ALL_SOURCE=1)
add_definitions(-DHAVE_DLFCN_H=1)
add_definitions(-DHAVE_PORTRANGE=1)
add_definitions(-D_TANDEM_SOURCE=1)
#add_definitions(-D__EXTENSIONS__=1)
add_definitions(-DHAVE_SCTP=1)
add_definitions(-D_GNU_SOURCE=1)
add_definitions(-DHAVE_LIBNICE_TCP=1)

find_package(GLib REQUIRED COMPONENTS gobject gio gthread)
find_package(LibWebSockets)
find_package(Jansson)
find_package(Nice)
find_package(GStreamer)
find_package(OpenSSL)
find_package(Usrsctp)

add_subdirectory(${CMAKE_SOURCE_DIR}/transport)
add_subdirectory(${CMAKE_SOURCE_DIR}/plugins)

include_directories(
   ${CMAKE_SOURCE_DIR}
   ${CMAKE_SOURCE_DIR}/transport
   ${GLIB_INCLUDE_DIRS}
   ${LIBWEBSOCKETS_INCLUDE_DIR}
   ${JANSSON_INCLUDE_DIRS}
   ${LIBNICE_INCLUDE_DIRS}
   ${GSTREAMER_INCLUDE_DIRS}
   ${OPENSSL_INCLUDE_DIR}
   ${SOFIASIP_INCLUDE_DIR}
   ${LIBUSRSCTP_INCLUDE_DIRS}
   ${SRTP_INCLUDE_DIR}
)
message("GLIB_GOBJECT_LIBRARIES: ${LIBNICE_INCLUDE_DIRS};${GLIBCONFIG_INCLUDE_DIR}")
set(PSCLIENT_LIBS 
   ${JANSSON_LIBRARIES} 
   ${LIBWEBSOCKETS_LIBRARIES} 
   ${GLIB_LIBRARIES} 
   ${LIBNICE_LIBRARIES} 
   ${GSTREAMER_LIBRARIES}
   ${OPENSSL_LIBRARIES}
   ${SOFIASIP_LIBRARIES}
   ${LIBUSRSCTP_LIBRARIES}
   ${GIO_LIBRARIES}
   ${GOBJECT_LIBRARIES}
   ${GTHREAD_LIBRARIES}
   ${SRTP_LIBRARIES}
#   gobject-2.0 gio-2.0 gthread-2.0
   dl m pthread)

message("PSCLIENT_LIBS: ${PSCLIENT_LIBS}")
   
set(PSCLIENT_SOURCES
   apierror.c
   config.c
   dtls.c
   dtls-bio.c
   ice.c
   psclient.c
   log.c
   rtcp.c
   sctp.c
   sdp.c
   turnrest.c
   utils.c)

message("CMAKE_C_FLAGS: ${CMAKE_C_FLAGS}")
add_executable(psclient ${PSCLIENT_SOURCES})
target_link_libraries(psclient ${PSCLIENT_LIBS})
install(TARGETS psclient RUNTIME DESTINATION ${CMAKE_INSTALL_PREFIX}/bin)
