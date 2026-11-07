# Tang Dynasty batch flow for the YADAN Board. Run through synth/build-yadan.sh.
import_device eagle_s20.db -package EG4S20NG88
read_verilog -file yadan_all.sv -top yadan_top
read_adc yadan.adc
optimize_rtl
report_area -file rtl.area
read_sdc yadan.sdc
optimize_gate
legalize_phy_inst
place
route
report_area -io_info -file phy.area
report_timing -file timing.rpt
export_db yadan.db
bitgen -bit yadan.bit
