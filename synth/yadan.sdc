create_clock -name clk_24mhz -period 41.667 [get_ports clk_24mhz]
derive_pll_clocks
