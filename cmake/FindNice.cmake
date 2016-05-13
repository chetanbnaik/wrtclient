# - Try to find libnice
# Once done this will define
#
#  LIBNICE_FOUND - system has Jansson
#  LIBNICE_INCLUDE_DIRS - the Jansson include directory
#  LIBNICE_LIBRARIES - Link these to use Jansson
#
#

if (LIBNICE_LIBRARIES AND LIBNICE_INCLUDE_DIRS)
  # in cache already
  set(LIBNICE_FOUND TRUE)
else (LIBNICE_LIBRARIES AND LIBNICE_INCLUDE_DIRS)
  find_path(LIBNICE_INCLUDE_DIR
    NAMES
      nice.h
    PATHS
      /usr/local/include/nice
      /usr/include/nice
      /opt/local/include/nice
      /sw/include/nice
  )

find_library(LIBNICE_LIBRARY
    NAMES
      nice
    PATHS
      /usr/local/lib
      /usr/lib/i386-linux-gnu
      /usr/lib
      /opt/local/lib
      /sw/lib
  )

set(LIBNICE_INCLUDE_DIRS
  ${LIBNICE_INCLUDE_DIR}
  )

if (LIBNICE_LIBRARY)
  set(LIBNICE_LIBRARIES
    ${LIBNICE_LIBRARIES}
    ${LIBNICE_LIBRARY}
    )
endif (LIBNICE_LIBRARY)

  include(FindPackageHandleStandardArgs)
  find_package_handle_standard_args(Nice DEFAULT_MSG
    LIBNICE_LIBRARIES LIBNICE_INCLUDE_DIRS)

  # show the LIBNICE_INCLUDE_DIRS and LIBNICE_LIBRARIES variables only in the advanced view
  mark_as_advanced(LIBNICE_INCLUDE_DIRS LIBNICE_LIBRARIES)

endif (LIBNICE_LIBRARIES AND LIBNICE_INCLUDE_DIRS)

