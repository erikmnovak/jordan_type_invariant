# Check real process exit status, stderr, and the entire output. An error must
# not leave behind a result which a pipeline could mistake for a valid answer.

function(check_ok expected)
  set(input_argument)
  if(DEFINED stdin_file)
    set(input_argument INPUT_FILE "${stdin_file}")
  endif()
  execute_process(COMMAND "${PROGRAM}" ${ARGN} ${input_argument}
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 20)
  if(NOT "${status}" STREQUAL "0" OR NOT "${output}" STREQUAL "${expected}" OR NOT "${error}" STREQUAL "")
    message(FATAL_ERROR "Unexpected success-path result (${status}) for ${ARGN}\nstdout: ${output}\nstderr: ${error}")
  endif()
endfunction()

function(check_error expected_message)
  execute_process(COMMAND "${PROGRAM}" ${ARGN}
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 20)
  if(NOT "${status}" STREQUAL "1" OR NOT "${output}" STREQUAL "" OR NOT error MATCHES "${expected_message}")
    message(FATAL_ERROR "Unexpected error-path result (${status}) for ${ARGN}\nstdout: ${output}\nstderr: ${error}")
  endif()
endfunction()

set(presentation "${EXAMPLES}/paper.scc")
set(slices "${EXAMPLES}/paper.slices")
set(expected "power ranks: 6 3 1 0\nJordan block counts: 1 1 1\n")
foreach(method IN ITEMS automatic rank-blocks streaming)
  check_ok("${expected}" type "${presentation}" "${slices}" --shift "0 0" --method "${method}")
endforeach()
check_ok("${expected}" type "${presentation}" "${slices}")
check_ok("filtered ranks: 6 3 1\n" ranks "${presentation}" "${slices}" --source "0 0" --target "0 0")
check_ok("filtered rank[1]: 3\n" ranks "${presentation}" "${slices}" --source "0 0" --target "0 0" --power 1)
check_ok("filtered rank[999]: 0\n" ranks "${presentation}" "${slices}" --source "0 0" --target "0 0" --power 999)
check_ok("power ranks: 0 0 0 0\nJordan block counts: 0 0 0\n" type "${presentation}" "${slices}" --shift "-100 -100")

set(stdin_file "${presentation}")
check_ok("${expected}" type - "${slices}")
set(stdin_file "${slices}")
check_ok("${expected}" type "${presentation}" -)
unset(stdin_file)

# A separate one-parameter fixture exercises off-diagonal transport rather than
# only equal-shift queries: the class dies at 2, making power-one transport zero.
check_ok("filtered ranks: 1 0\n" ranks "${CASES}/death_transport.scc"
  "${CASES}/line_pair.slices" --source 0 --target 1)
check_error("different parameter counts" type "${presentation}" "${CASES}/line_pair.slices")

check_error("chain_complex.scc: Line 3:.*chain complex" type
  "${CASES}/readers/invalid/chain_complex.scc" "${slices}")
check_error("unknown_header.scc: Line 1: Expected scc2020" type
  "${CASES}/readers/invalid/unknown_header.scc" "${slices}")
check_error("Cannot open" type "${CASES}/missing.scc" "${slices}")
check_error("Cannot open" type "${presentation}" "${CASES}/missing.slices")
check_error("Only one input" type - -)
check_error("Expected a query command")
check_error("Command must" unknown "${presentation}" "${slices}")
check_error("requires both" ranks "${presentation}" "${slices}" --source "0 0")
check_error("Unknown option" type "${presentation}" "${slices}" --target "0 0")
check_error("Unknown option" ranks "${presentation}" "${slices}" --method streaming)
check_error("Repeated option" type "${presentation}" "${slices}" --shift "0 0" --shift "1 1")
check_error("Missing value" type "${presentation}" "${slices}" --shift)
check_error("Wrong number" type "${presentation}" "${slices}" --shift "0")
check_error("finite decimal" type "${presentation}" "${slices}" --shift "0 nan")
check_error("finite decimal" type "${presentation}" "${slices}" --shift "0 0 1e")
check_error("Invalid coordinate" type "${presentation}" "${slices}" --shift "+-1 0")
check_error("--method must" type "${presentation}" "${slices}" --method invalid)
check_error("nonnegative integer" ranks "${presentation}" "${slices}" --source "0 0" --target "0 0" --power -1)
check_error("nonnegative integer" ranks "${presentation}" "${slices}" --source "0 0" --target "0 0" --power 1junk)
check_error("source.*target|Source.*target|comparable|coordinatewise" ranks "${presentation}" "${slices}" --source "1 0" --target "0 1")

execute_process(COMMAND "${PROGRAM}" --help RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT "${status}" STREQUAL "0" OR NOT output MATCHES "jordan_type ranks" OR NOT "${error}" STREQUAL "")
  message(FATAL_ERROR "Help command failed: ${status} ${error}")
endif()
