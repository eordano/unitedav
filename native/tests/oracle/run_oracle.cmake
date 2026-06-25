# SPDX-License-Identifier: Apache-2.0
# Oracle ctest wrapper: registers per-clip/per-mode ctests and a standalone
# cmake -P runner.

function(_uav_oracle_is_hw_decodable base out_var)
  if(base MATCHES "h264" OR base MATCHES "hevc" OR base MATCHES "av1")
    set(${out_var} TRUE PARENT_SCOPE)
  else()
    set(${out_var} FALSE PARENT_SCOPE)
  endif()
endfunction()

function(_uav_oracle_has_video base out_var)
  if(base MATCHES "novideo")
    set(${out_var} FALSE PARENT_SCOPE)
  else()
    set(${out_var} TRUE PARENT_SCOPE)
  endif()
endfunction()

function(uav_register_oracle_tests)
  if(NOT TARGET uav_oracle)
    message(STATUS "uav_register_oracle_tests: uav_oracle target absent; skipping.")
    return()
  endif()
  set(_media "${CMAKE_SOURCE_DIR}/../tests/media/out")
  if(DEFINED UAV_MEDIA_DIR)
    set(_media "${UAV_MEDIA_DIR}")
  endif()
  file(GLOB _clips
       "${_media}/*.mp4" "${_media}/*.mov" "${_media}/*.mkv"
       "${_media}/*.webm" "${_media}/*.ts" "${_media}/*.ogg")
  if(NOT _clips)
    message(STATUS "uav_register_oracle_tests: no clips in ${_media} "
                   "(run tests/media/gen.sh); per-clip oracle cases not registered.")
    return()
  endif()
  foreach(_clip ${_clips})
    get_filename_component(_base "${_clip}" NAME_WE)
    _uav_oracle_has_video("${_base}" _hasvid)
    _uav_oracle_is_hw_decodable("${_base}" _hwdec)

    add_test(NAME "oracle.swref.${_base}"
             COMMAND uav_oracle "${_clip}" --mode sw-vs-ref)
    set_tests_properties("oracle.swref.${_base}" PROPERTIES
      TIMEOUT 120 LABELS "oracle;swref" SKIP_RETURN_CODE 77)

    if(_hasvid)
      add_test(NAME "oracle.contract.${_base}"
               COMMAND uav_oracle "${_clip}" --mode contract)
      set_tests_properties("oracle.contract.${_base}" PROPERTIES
        TIMEOUT 120 LABELS "oracle;contract" SKIP_RETURN_CODE 77)

      if(_hwdec)
        add_test(NAME "oracle.hwsw.${_base}"
                 COMMAND uav_oracle "${_clip}" --mode hw-vs-sw)
        set_tests_properties("oracle.hwsw.${_base}" PROPERTIES
          TIMEOUT 180 LABELS "oracle;hwsw" SKIP_RETURN_CODE 77)
      endif()
    endif()
  endforeach()
  message(STATUS "uav_register_oracle_tests: registered per-clip oracle cases from ${_media}")
endfunction()

if(CMAKE_SCRIPT_MODE_FILE)
  if(NOT DEFINED UAV_ORACLE_BIN)
    set(UAV_ORACLE_BIN "native/build/uav_oracle")
  endif()
  if(NOT DEFINED UAV_MEDIA_DIR)
    set(UAV_MEDIA_DIR "tests/media/out")
  endif()
  if(NOT EXISTS "${UAV_ORACLE_BIN}")
    message(FATAL_ERROR "uav_oracle binary not found: ${UAV_ORACLE_BIN} (build native/ first)")
  endif()

  file(GLOB _clips
       "${UAV_MEDIA_DIR}/*.mp4" "${UAV_MEDIA_DIR}/*.mov" "${UAV_MEDIA_DIR}/*.mkv"
       "${UAV_MEDIA_DIR}/*.webm" "${UAV_MEDIA_DIR}/*.ts" "${UAV_MEDIA_DIR}/*.ogg")
  if(NOT _clips)
    message(FATAL_ERROR "no clips in ${UAV_MEDIA_DIR} (run tests/media/gen.sh)")
  endif()

  set(_fail 0)
  set(_pass 0)
  set(_skip 0)
  foreach(_clip ${_clips})
    get_filename_component(_base "${_clip}" NAME_WE)
    _uav_oracle_has_video("${_base}" _hasvid)
    _uav_oracle_is_hw_decodable("${_base}" _hwdec)
    set(_modes "sw-vs-ref")
    if(_hasvid)
      list(APPEND _modes "contract")
      if(_hwdec)
        list(APPEND _modes "hw-vs-sw")
      endif()
    endif()
    foreach(_mode ${_modes})
      execute_process(COMMAND "${UAV_ORACLE_BIN}" "${_clip}" --mode "${_mode}"
                      RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _errout)
      if(_rc EQUAL 0)
        set(_verdict "PASS")
        math(EXPR _pass "${_pass}+1")
      elseif(_rc EQUAL 77)
        set(_verdict "SKIP")
        math(EXPR _skip "${_skip}+1")
      else()
        set(_verdict "FAIL")
        math(EXPR _fail "${_fail}+1")
        message("---- ${_base} (${_mode}) FAIL (rc=${_rc}) ----")
        message("${_out}${_errout}")
      endif()
      message("  ${_verdict}  ${_base}  [${_mode}]")
    endforeach()
  endforeach()
  message("oracle matrix: PASS=${_pass} SKIP=${_skip} FAIL=${_fail}")
  if(_fail GREATER 0)
    message(FATAL_ERROR "oracle: ${_fail} clip/mode case(s) FAILED")
  endif()
endif()
