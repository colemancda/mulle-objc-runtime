# can be included multiple times

if( MULLE_TRACE_INCLUDE)
   message( STATUS "# Include \"${CMAKE_CURRENT_LIST_FILE}\"" )
endif()

include( AllLoadC)

CreateForceAllLoadList( ALL_LOAD_DEPENDENCY_LIBRARIES FORCE_ALL_LOAD_DEPENDENCY_LIBRARIES)

set( EXECUTABLE_LIBRARY_LIST
   ${FORCE_ALL_LOAD_DEPENDENCY_LIBRARIES}
   ${EXECUTABLE_LIBRARY_LIST}
)

include( ExecutableCAux OPTIONAL)
