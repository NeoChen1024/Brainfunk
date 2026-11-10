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
# value from 0.8 to 0.95 closed timing on the same design where 1.0 did not; after a change to the
# loader 0.9 missed by 0.04 ns in the core, and 0.85 had the most slack. Which setting wins moves with
# any change: the scalar core met it at 0.8 (+0.023 ns) and missed at 0.85, and once brainfunk_system
# was split out of the board top, it meets it at 0.75 (+0.083), 0.85 (+0.023) and 1.0 (+0.034) and
# misses at 0.8 (-0.197) and 0.9 (-0.437), all in short paths into the data cache. With the tape in the
# sixteen BRAM32K in place of the data cache, it meets it at 0.8 (+0.001) and 0.95 (+0.001) and misses at
# 0.75 (-0.232), 0.85 (-0.278), 0.9 (-0.252) and 1.0 (-0.217), in the tape's write data and the paths
# from its rows. Higher place effort and opt_timing made place stop part way, with no message.
# The same gates place differently by how the RTL names its registers. With M0 to X each one packed
# struct, flags first, none of 36 values from 0.70 to 1.05 met it (-0.149 at best); with the struct's
# wide fields first, 0.75 does (+0.001) and seven others miss by 0.18 to 0.46. With a register to a
# field, as before the struct, 0.85 (+0.017) and 0.92 (+0.006) met it, of 20.
set_param place relaxation 0.75
# Finishes, then raises an error with an empty message on a design with the SDRAM in it.
catch {place}
route
report_area -io_info -file phy.area
report_timing -file timing.rpt
export_db yadan.db
bitgen -bit yadan.bit
