# Run as a script (cmake -P) after building lila: THIRD-PARTY-NOTICES.md beside the executable and each component's full
# license text in licenses/<id>.txt, which Help > Licenses shows. -DSOURCE_DIR, -DSHARE_DIR (vcpkg's share), -DOUT_DIR.
file(MAKE_DIRECTORY "${OUT_DIR}/licenses")
file(COPY_FILE "${SOURCE_DIR}/THIRD-PARTY-NOTICES.md" "${OUT_DIR}/THIRD-PARTY-NOTICES.md" ONLY_IF_DIFFERENT)
file(COPY_FILE "${SOURCE_DIR}/LICENSE" "${OUT_DIR}/LICENSE" ONLY_IF_DIFFERENT)
if(SHARE_DIR AND IS_DIRECTORY "${SHARE_DIR}")
    file(GLOB _ports LIST_DIRECTORIES true "${SHARE_DIR}/*")
    foreach(_port ${_ports})
        if(EXISTS "${_port}/copyright")
            get_filename_component(_id "${_port}" NAME)
            file(COPY_FILE "${_port}/copyright" "${OUT_DIR}/licenses/${_id}.txt" ONLY_IF_DIFFERENT)
        endif()
    endforeach()
endif()
foreach(_pair
        "cgltf|third_party/cgltf/LICENSE"
        "dem-bones|third_party/dem-bones/LICENSE.md"
        "archivo|resources/fonts/Archivo-LICENSE.txt"
        "inter|resources/fonts/Inter-LICENSE.txt"
        "jetbrains-mono|resources/fonts/JetBrainsMono-LICENSE.txt"
        "tabler-icons|resources/icons/tabler/LICENSE")
    string(REPLACE "|" ";" _parts "${_pair}")
    list(GET _parts 0 _id)
    list(GET _parts 1 _file)
    if(EXISTS "${SOURCE_DIR}/${_file}")
        file(COPY_FILE "${SOURCE_DIR}/${_file}" "${OUT_DIR}/licenses/${_id}.txt" ONLY_IF_DIFFERENT)
    endif()
endforeach()
