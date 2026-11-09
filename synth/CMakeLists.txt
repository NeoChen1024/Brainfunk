# The YADAN Board flow, with Tang Dynasty from TD_HOME:
#
#   yadan        synthesize, place and route: build/yadan/yadan.bit and its reports
#   yadan-load   configure the FPGA over JTAG; the board's flash is not touched
set(TD_HOME "$ENV{TD_HOME}" CACHE PATH "Tang Dynasty install directory")
set(YADAN_MHZ 168 CACHE STRING "The board's clock in MHz, a multiple of 24; 168 is where timing closes")
set(YADAN_SDRAM_DEGREES 240 CACHE STRING "The SDRAM clock's phase after the core's, measured for YADAN_MHZ")
set(YADAN_SDRAM_CAPTURE 4 CACHE STRING "Cycles from READ to data at YADAN_SDRAM_DEGREES")
set(YADAN_WORK ${PROJECT_BINARY_DIR}/yadan)

file(GLOB YADAN_RTL CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/rtl/*.sv ${PROJECT_SOURCE_DIR}/rtl/board/*.sv)
add_custom_command(
    OUTPUT ${YADAN_WORK}/yadan.bit
    COMMAND ${CMAKE_COMMAND} -E env TD_HOME=${TD_HOME} YADAN_WORK=${YADAN_WORK} YADAN_MHZ=${YADAN_MHZ}
            YADAN_SDRAM_DEGREES=${YADAN_SDRAM_DEGREES} YADAN_SDRAM_CAPTURE=${YADAN_SDRAM_CAPTURE} sh ${CMAKE_CURRENT_SOURCE_DIR}/build-yadan.sh
    DEPENDS ${YADAN_RTL} build-yadan.sh yadan.tcl yadan.adc yadan.sdc
    COMMENT "Tang Dynasty: synthesis, place and route"
    USES_TERMINAL)
add_custom_target(yadan DEPENDS ${YADAN_WORK}/yadan.bit)

add_custom_target(yadan-load
    COMMAND ${CMAKE_COMMAND} -E env TD_HOME=${TD_HOME} YADAN_WORK=${YADAN_WORK} sh ${CMAKE_CURRENT_SOURCE_DIR}/load-yadan.sh
    DEPENDS yadan
    COMMENT "Loading the bitstream over JTAG"
    USES_TERMINAL)
