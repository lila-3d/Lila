lila_module(lila_media Scene
    Decode.cpp Decode.h DecodeBackend.h
    DecodeMF.cpp
    Media.cpp Media.h
    SoundCues.cpp SoundCues.h
    AudioEngine.cpp AudioEngine.h)

# FFmpeg is LGPL: linked as DLLs that ship beside lila.exe, built without gpl or nonfree parts and without the H.264 and
# HEVC decoders (vcpkg-ports/ffmpeg), which Windows decodes through Media Foundation instead.
find_package(FFMPEG REQUIRED)
# miniaudio is a single header; AudioEngine.cpp compiles its implementation.
find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h REQUIRED)
target_include_directories(lila_media PRIVATE ${FFMPEG_INCLUDE_DIRS} ${MINIAUDIO_INCLUDE_DIR})
target_link_directories(lila_media PUBLIC ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(lila_media PUBLIC lila_scene PRIVATE ${FFMPEG_LIBRARIES})
if(WIN32)
    target_link_libraries(lila_media PRIVATE mfplat mfreadwrite mfuuid ole32 propsys)
endif()
