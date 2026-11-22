include_guard(GLOBAL)

option(MIRAGE_DOCS_HTML "Generate searchable Doxygen HTML" ON)
option(MIRAGE_DOCS_XML "Generate Doxygen XML and enable the coverage check" ON)
option(MIRAGE_DOCS_WEBSITE "Enable the MkDocs Material website target" OFF)
option(MIRAGE_DOCS_SDK "Include generated UE3 SDK headers in Doxygen" OFF)
option(MIRAGE_DOCS_LUA "Generate MirageAPIGen's runtime API reference for the website" ON)
option(MIRAGE_DOCS_LUA_INSPECTION "Include SDK inspection pages in the Lua reference" OFF)
set(MIRAGE_DOCS_OUTPUT "${CMAKE_BINARY_DIR}/documentation" CACHE PATH "Generated documentation directory")
set(MIRAGE_DOCS_VERSION "" CACHE STRING "Version label override for generated documentation")
set(MIRAGE_DOCS_LUA_INPUT "" CACHE FILEPATH "Optional MirageAPIGen SDK metadata file for the website")
set(MIRAGE_DOCS_LUA_CONFIG "" CACHE FILEPATH "Optional MirageAPIGen TOML policy for the website")

function(mirage_add_documentation)
  cmake_parse_arguments(PARSE_ARGV 0 DOC "" "VERSION" "")
  if(NOT MIRAGE_DOCS_HTML AND NOT MIRAGE_DOCS_XML)
    message(FATAL_ERROR "Enable at least one of MIRAGE_DOCS_HTML and MIRAGE_DOCS_XML")
  endif()
  if(MIRAGE_DOCS_WEBSITE AND NOT MIRAGE_DOCS_HTML)
    message(FATAL_ERROR "MIRAGE_DOCS_WEBSITE requires MIRAGE_DOCS_HTML")
  endif()

  find_package(Doxygen 1.9.8 REQUIRED OPTIONAL_COMPONENTS dot)
  find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
  set(_doc_have_dot NO)
  if(TARGET Doxygen::dot)
    set(_doc_have_dot YES)
    get_filename_component(_doc_dot_dir "${DOXYGEN_DOT_EXECUTABLE}" DIRECTORY)
  else()
    message(STATUS "Graphviz dot unavailable. Doxygen will use built-in class diagrams.")
  endif()

  get_filename_component(_doc_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
  cmake_path(ABSOLUTE_PATH MIRAGE_DOCS_OUTPUT BASE_DIRECTORY "${CMAKE_BINARY_DIR}" NORMALIZE OUTPUT_VARIABLE _doc_out)
  set(_doc_doxygen "${_doc_out}/doxygen")
  set(_doc_tools "${_doc_root}/tools/documentation")
  set(_doc_version "${DOC_VERSION}")
  if(MIRAGE_DOCS_VERSION)
    set(_doc_version "${MIRAGE_DOCS_VERSION}")
  endif()
  foreach(_bool IN ITEMS MIRAGE_DOCS_HTML MIRAGE_DOCS_XML)
    if(${_bool})
      set(_${_bool} YES)
    else()
      set(_${_bool} NO)
    endif()
  endforeach()
  if(MIRAGE_DOCS_SDK)
    set(_doc_sdk "sdk/include/sdk")
    # Keep opt-in SDK pages out of a later default website build.
    string(APPEND _doc_doxygen "-sdk")
  endif()
  set(_doc_filter "\\\"${Python3_EXECUTABLE}\\\" \\\"${_doc_tools}/filter_api.py\\\"")
  if(CMAKE_HOST_WIN32)
    # Doxygen starts filters through cmd.exe, which strips a leading quoted executable.
    set(_doc_filter "call ${_doc_filter}")
  endif()
  configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/Doxyfile.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile" @ONLY)

  add_custom_target(docs
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_doc_doxygen}"
    COMMAND Doxygen::doxygen "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
    WORKING_DIRECTORY "${_doc_root}"
    COMMENT "Generating Mirage Doxygen documentation"
    VERBATIM)

  if(MIRAGE_DOCS_XML)
    add_custom_target(docs-check
      COMMAND Python3::Interpreter "${_doc_tools}/coverage.py"
        "${_doc_doxygen}/xml" --output "${_doc_out}/coverage.json"
        --baseline "${_doc_tools}/coverage-baseline.json"
      DEPENDS docs
      COMMENT "Checking public API documentation coverage"
      VERBATIM)
  endif()

  if(MIRAGE_DOCS_WEBSITE)
    execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import mkdocs, material"
                    RESULT_VARIABLE _doc_mkdocs ERROR_QUIET)
    if(NOT _doc_mkdocs STREQUAL "0")
      message(FATAL_ERROR "Install tools/documentation/requirements.txt in the CMake Python environment")
    endif()
    set(_doc_site_args)
    if(NOT MIRAGE_DOCS_LUA)
      list(APPEND _doc_site_args --no-lua)
    endif()
    if(MIRAGE_DOCS_LUA_INSPECTION)
      list(APPEND _doc_site_args --lua-inspection)
    endif()
    if(MIRAGE_DOCS_LUA_INPUT)
      list(APPEND _doc_site_args --lua-input "${MIRAGE_DOCS_LUA_INPUT}")
    endif()
    if(MIRAGE_DOCS_LUA_CONFIG)
      list(APPEND _doc_site_args --lua-config "${MIRAGE_DOCS_LUA_CONFIG}")
    endif()
    set(_doc_site_dependency docs)
    if(TARGET docs-check)
      set(_doc_site_dependency docs-check)
      list(APPEND _doc_site_args --coverage "${_doc_out}/coverage.json")
    endif()
    add_custom_target(docs-site
      COMMAND Python3::Interpreter "${_doc_tools}/site.py"
        --output "${_doc_out}" --doxygen "${_doc_doxygen}/html" --version "${_doc_version}" ${_doc_site_args}
      DEPENDS ${_doc_site_dependency}
      WORKING_DIRECTORY "${_doc_root}"
      COMMENT "Building and validating the Mirage documentation website"
      VERBATIM)
  endif()
endfunction()
