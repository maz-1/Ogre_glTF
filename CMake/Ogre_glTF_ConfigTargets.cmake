function(Ogre_glTF_config_sample PROJECTNAME)
    file(GLOB SOURCES ./*.cpp ./*.hpp ./include/*.hpp)

    add_executable(${PROJECTNAME} WIN32 MACOSX_BUNDLE ${SAMPLES_COMMON_HEADER_FILES} ${SAMPLES_COMMON_SOURCE_FILES} ${SOURCES})
    add_dependencies(${PROJECTNAME} Ogre_glTF)

    target_include_directories( ${PROJECTNAME} PUBLIC
        #Ogre and the physics based high level material system
        ${OGRE_INCLUDE_DIRS}
        ${OGRE_HlmsPbs_INCLUDE_DIRS}
        ${OGRE_INCLUDE_DIR}/Hlms/Common
        ${CMAKE_SOURCE_DIR}/Samples/Common
    )

    target_link_libraries(${PROJECTNAME}
        ${OGRE_LIBRARIES}
        ${OGRE_HlmsPbs_LIBRARIES}
        Ogre_glTF
    )

    if (Ogre_glTF_STATIC AND UNIX)
        target_link_libraries(${PROJECTNAME}
            ${OGRE_RenderSystem_GL3Plus_LIBRARIES}
        )

        if (NOT APPLE)
            target_link_libraries(${PROJECTNAME}
                dl
                pthread
                Xt
                X11
                Xaw
                Xrandr
            )
        endif ()
    endif ()
	#set( LIBRARY_OUTPUT_PATH "${CMAKE_CURRENT_SOURCE_DIR}/bin" )
	message("LIBRARY_OUTPUT_PATH :" ${LIBRARY_OUTPUT_PATH})
	message("EXECUTABLE_OUTPUT_PATH :" ${EXECUTABLE_OUTPUT_PATH})
	message("CMAKE_CURRENT_SOURCE_DIR: " ${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(${PROJECTNAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${LIBRARY_OUTPUT_PATH}"
	)
    set_property(TARGET ${PROJECTNAME} PROPERTY VS_DEBUGGER_WORKING_DIRECTORY ${LIBRARY_OUTPUT_PATH}/$(Configuration))
endfunction(Ogre_glTF_config_sample)
