lila_module(lila_media Scene
    Decode.cpp Decode.h
    Media.cpp Media.h
    SoundCues.cpp SoundCues.h
    AudioEngine.cpp AudioEngine.h)

# FFmpeg is LGPL: linked as DLLs that ship beside lila.exe, built without gpl or nonfree parts (see vcpkg.json).
find_package(FFMPEG REQUIRED)
# miniaudio is a single header; AudioEngine.cpp compiles its implementation.
find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h REQUIRED)
target_include_directories(lila_media PRIVATE ${FFMPEG_INCLUDE_DIRS} ${MINIAUDIO_INCLUDE_DIR})
target_link_directories(lila_media PUBLIC ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(lila_media PUBLIC lila_scene PRIVATE ${FFMPEG_LIBRARIES})
