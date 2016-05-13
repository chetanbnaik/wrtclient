include_directories(
   ${CMAKE_SOURCE_DIR}/transport
   ${GLIB_INCLUDE_DIRS}
   ${LIBWEBSOCKETS_INCLUDE_DIR}
   ${JANSSON_INCLUDE_DIRS}
)

set(TRANSPORT_LIBS ${JANSSON_LIBRARIES} ${LIBWEBSOCKETS_LIBRARIES} ${GLIB_LIBRARIES} pthread)
add_library(pswebsockets SHARED ps_websockets.c)

target_link_libraries(pswebsockets ${TRANSPORT_LIBS})
set(PS_TRANSPORT_LIBRARY ${LIBRARY_OUTPUT_PATH}/libpswebsockets.so)
install(TARGETS pswebsockets LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/lib)
