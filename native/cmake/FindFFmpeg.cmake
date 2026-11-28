# The same ABI must be used by Qt Multimedia and the shared source decoder.
set(FFmpeg_ROOT "$ENV{FFmpeg_ROOT}" CACHE PATH "FFmpeg development package (include, lib and bin)")
find_path(FFmpeg_INCLUDE_DIR libavformat/avformat.h HINTS "${FFmpeg_ROOT}/include")
set(FFmpeg_LIBRARIES "")
foreach(component avformat avcodec avutil swscale)
    find_library(FFmpeg_${component}_LIBRARY NAMES ${component} HINTS "${FFmpeg_ROOT}/lib")
    list(APPEND FFmpeg_LIBRARIES "${FFmpeg_${component}_LIBRARY}")
endforeach()
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FFmpeg REQUIRED_VARS FFmpeg_INCLUDE_DIR
    FFmpeg_avformat_LIBRARY FFmpeg_avcodec_LIBRARY FFmpeg_avutil_LIBRARY FFmpeg_swscale_LIBRARY)
if(FFmpeg_FOUND AND NOT TARGET FFmpeg::FFmpeg)
    add_library(FFmpeg::FFmpeg INTERFACE IMPORTED)
    set_target_properties(FFmpeg::FFmpeg PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${FFmpeg_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "${FFmpeg_LIBRARIES}")
endif()
