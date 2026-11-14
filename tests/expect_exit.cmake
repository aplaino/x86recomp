# Runs a test program (CMD, |-separated) and passes only when it exits
# with EXIT and its output matches OUTPUT. A test that must fail (a
# planted bug caught, an input rejected) passes on its own detection
# that way, not on a crash, a usage error or any other non-zero exit; a
# test checked by its output passes only when it also exits as it should.
string(REPLACE "|" ";" _cmd "${CMD}")
execute_process(COMMAND ${_cmd} RESULT_VARIABLE _r OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
message("${_out}")
if(NOT _r STREQUAL "${EXIT}")
  message(FATAL_ERROR "exited with ${_r}, not ${EXIT}")
endif()
if(NOT _out MATCHES "${OUTPUT}")
  message(FATAL_ERROR "no output matches \"${OUTPUT}\"")
endif()
