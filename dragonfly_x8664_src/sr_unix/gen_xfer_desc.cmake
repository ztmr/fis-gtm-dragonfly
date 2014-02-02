#################################################################
#								#
#	Copyright 2012 Fidelity Information Services, Inc	#
#								#
#	This source code contains the intellectual property	#
#	of its copyright holder(s), and is made available	#
#	under a license.  If you do not know the terms of	#
#	the license, please stop and do not read further.	#
#								#
#################################################################
set(I "")
foreach(i ${includes})
  list(APPEND I "-I${i}")
endforeach()
file(WRITE tmp_xfer_1.c "
/* We have not yet created gtm_threadgbl_deftypes.h and don't need it, signal gtm_threadgbl.h to avoid including it */
#define NO_THREADGBL_DEFTYPES
#include \"mdef.h\"
#define XFER(a,b) MY_XF,b
#include \"xfer.h\"
")

execute_process(
  COMMAND ${CMAKE_C_COMPILER} ${I} -E tmp_xfer_1.c -o tmp_xfer_2.c
  RESULT_VARIABLE failed
  )
if(failed)
  message(FATAL_ERROR "Preprocessing with ${CMAKE_C_COMPILER} failed")
endif()
file(STRINGS tmp_xfer_2.c lines REGEX "MY_XF")
string(REGEX REPLACE "(MY_XF|,)" "" names "${lines}")
file(REMOVE tmp_xfer_1.c tmp_xfer_2.c)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/sources.list" sources)

set(ftypes "")
#set(defines "")
foreach(name ${names})
  set(ftype "")
  if(";${sources};" MATCHES ";([^;]*/${name}\\.s);")
    set(ftype GTM_ASM_RTN)
  elseif(";${sources};" MATCHES ";([^;]*/${name}\\.c);")
    file(STRINGS "${CMAKE_MATCH_1}" sig REGEX "${name}.*\\.\\.\\.")
    if(sig)
      set(ftype "GTM_C_VAR_ARGS_RTN")
    else()
      set(ftype "GTM_C_RTN")
    endif()
  endif()
  if(NOT ftype)
    set(ftype GTM_C_VAR_ARGS_RTN)
    foreach(src ${sources})
      if("${src}" MATCHES "\\.s$")
        file(STRINGS "${src}" sig REGEX "^${name}")
        if(sig)
          set(ftype GTM_ASM_RTN)
          break()
        endif()
      endif()
    endforeach()
  endif()
  set(ftypes "${ftypes}${ftype}, /* ${name} */ \\\n")
  #set(defines "${defines}#define ${name}_FUNCTYPE ${ftype}\n") # TODO for ia64
endforeach()

file(WRITE xfer_desc.i "/* Generated by gen_xfer_desc.cmake */
#define GTM_C_RTN 1
#define GTM_ASM_RTN 2
#define GTM_C_VAR_ARGS_RTN 3
#define DEFINE_XFER_TABLE_DESC char xfer_table_desc[] = \\
{ \\
${ftypes}0}
 \n")