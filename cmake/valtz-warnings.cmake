# valtz_target_warnings(<target>) -- the warning set for Valtz's own C++.
# Third-party code (LMDB, nlohmann, vpipe headers) comes in as SYSTEM
# includes and is not held to it.

function(valtz_target_warnings target)
  target_compile_options(${target} PRIVATE
    $<$<COMPILE_LANGUAGE:C,CXX,OBJCXX>:-Wall -Wextra -Wno-unused-parameter>
    $<$<AND:$<BOOL:${VALTZ_WERROR}>,$<COMPILE_LANGUAGE:C,CXX,OBJCXX>>:-Werror>)
endfunction()
