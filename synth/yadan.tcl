# Tang Dynasty batch flow for the YADAN Board. Run through synth/build-yadan.sh.
import_device eagle_s20.db -package EG4S20NG88
read_verilog -file yadan_all.sv -top yadan_top
read_adc yadan.adc
optimize_rtl
report_area -file rtl.area
read_sdc yadan.sdc
optimize_gate
legalize_phy_inst
# The design runs close to the part's limit, and with the default relaxation of 1.0 a small change could
# leave paths with no logic in them, block RAM straight to a register, failing on placement alone. Any
# value from 0.8 to 0.95 closed timing on the same design where 1.0 did not. Higher place effort and
# opt_timing made place stop part way, with no message.
set_param place relaxation 0.9
# Finishes, then raises an error with an empty message on a design with the SDRAM in it.
catch {place}
route
report_area -io_info -file phy.area
report_timing -file timing.rpt
export_db yadan.db
bitgen -bit yadan.bit
