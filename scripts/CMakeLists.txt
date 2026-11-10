option(L3D_BUILD_GAME_SCRIPTS "Build and publish the C# NativeAOT gameplay plugin" ON)

if(L3D_BUILD_GAME_SCRIPTS)
    find_program(DOTNET_EXECUTABLE NAMES dotnet)
    if(DOTNET_EXECUTABLE)
        add_custom_target(game_scripts ALL
            COMMAND "${DOTNET_EXECUTABLE}" publish
                "${CMAKE_SOURCE_DIR}/scripts/GameScripts/GameScripts.csproj"
                -c Release
                -r win-x64
                --self-contained true
                -o "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/scripts/GameScripts"
            COMMENT "Publishing C# gameplay scripts with NativeAOT"
            VERBATIM
            USES_TERMINAL
        )
    else()
        message(WARNING "dotnet was not found; GameScripts.dll will not be built")
    endif()
endif()
