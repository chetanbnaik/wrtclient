include_directories(
   ${CMAKE_SOURCE_DIR}/plugins
   ${GLIB_INCLUDE_DIRS}
   ${LIBWEBSOCKETS_INCLUDE_DIR}
   ${JANSSON_INCLUDE_DIRS}
   ${GSTREAMER_INCLUDE_DIRS}
)

set(TRANSPORT_LIBS 
   ${JANSSON_LIBRARIES} 
   ${LIBWEBSOCKETS_LIBRARIES} 
   ${GLIB_LIBRARIES} 
   ${GSTREAMER_LIBRARIES} 
   pthread)
   
add_library(psgstreamer SHARED ps_gstreamer.c)

target_link_libraries(psgstreamer ${TRANSPORT_LIBS})
set(PS_WEBRTC_LIBRARY ${LIBRARY_OUTPUT_PATH}/libpsgstreamer.so)
install(TARGETS psgstreamer LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/lib)
