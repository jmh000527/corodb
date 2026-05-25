# 在编译完成后，若构建目录下尚不存在 corodb.conf，则使用 corodb_genconfig
# 写出一份默认配置，供用户手动编辑。已存在的配置文件不会被覆盖。
if(NOT EXISTS "${CONF_PATH}")
    execute_process(
        COMMAND "${EXE}" "${CONF_PATH}"
        RESULT_VARIABLE _rc
    )
    if(NOT _rc EQUAL 0)
        message(WARNING "Failed to generate default config at ${CONF_PATH} (rc=${_rc})")
    else()
        message(STATUS "Generated default corodb.conf at ${CONF_PATH}")
    endif()
endif()
