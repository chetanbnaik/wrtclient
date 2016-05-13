# - Try to find libnice
# Once done this will define
#
#  LIBUSRSCTP_FOUND - system has Jansson
#  LIBUSRSCTP_INCLUDE_DIRS - the Jansson include directory
#  LIBUSRSCTP_LIBRARIES - Link these to use Jansson
#
#

if (LIBUSRSCTP_LIBRARIES AND LIBUSRSCTP_INCLUDE_DIRS)
  # in cache already
  set(LIBUSRSCTP_FOUND TRUE)
else (LIBUSRSCTP_LIBRARIES AND LIBUSRSCTP_INCLUDE_DIRS)
  find_path(LIBUSRSCTP_INCLUDE_DIR
    NAMES
      usrsctp.h
    PATHS
      /usr/include
      /usr/local/include
      /opt/local/include
      /sw/include
  )

find_library(LIBUSRSCTP_LIBRARY
    NAMES
      usrsctp
    PATHS
      /usr/lib
      /usr/lib/i386-linux-gnu
      /usr/local/lib
      /opt/local/lib
      /sw/lib
  )

set(LIBUSRSCTP_INCLUDE_DIRS
  ${LIBUSRSCTP_INCLUDE_DIR}
  )

if (LIBUSRSCTP_LIBRARY)
  set(LIBUSRSCTP_LIBRARIES
    ${LIBUSRSCTP_LIBRARIES}
    ${LIBUSRSCTP_LIBRARY}
    )
endif (LIBUSRSCTP_LIBRARY)

  include(FindPackageHandleStandardArgs)
  find_package_handle_standard_args(Usrsctp DEFAULT_MSG
    LIBUSRSCTP_LIBRARIES LIBUSRSCTP_INCLUDE_DIRS)

  # show the LIBUSRSCTP_INCLUDE_DIRS and LIBUSRSCTP_LIBRARIES variables only in the advanced view
  mark_as_advanced(LIBUSRSCTP_INCLUDE_DIRS LIBUSRSCTP_LIBRARIES)

endif (LIBUSRSCTP_LIBRARIES AND LIBUSRSCTP_INCLUDE_DIRS)

