# Copies SRC to DST only when DST does not exist yet.
#
# Used for the ReShade configuration: it holds the RenoDX Neural Rendering slider
# values, which the user edits in the overlay. copy_if_different would throw those
# away on every rebuild.
if(NOT EXISTS "${DST}")
    file(COPY_FILE "${SRC}" "${DST}")
    message(STATUS "Staged ${DST}")
endif()
