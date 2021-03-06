# Inspired from /usr/share/autoconf/autoconf/c.m4
FOREACH(KEYWORD "__inline__" "__inline" "inline")
   IF(NOT DEFINED C_INLINE)
     TRY_COMPILE(C_HAS_${KEYWORD} "${AeonWave_BINARY_DIR}"
       "${AeonWave_SOURCE_DIR}/src/test_inline.c"
       COMPILE_DEFINITIONS "-Dinline=${KEYWORD}")
     IF(C_HAS_${KEYWORD})
       SET(C_INLINE "ON")
       ADD_DEFINITIONS("-Dinline=${KEYWORD}")
       message("-- Found inline keywoard ${KEYWORD}")
     ENDIF(C_HAS_${KEYWORD})
   ENDIF(NOT DEFINED C_INLINE)
ENDFOREACH(KEYWORD)
IF(NOT DEFINED C_INLINE)
   ADD_DEFINITIONS("-Dinline=")
ENDIF(NOT DEFINED C_INLINE)
