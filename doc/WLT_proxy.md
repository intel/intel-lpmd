
WLT (workload type) proxy hints use predefined CPU utilization thresholds and software algorithm to retrieve and detect WLT (same as WLT hints from hardware (WLTEnable)). When proxy hints detection (WLTProxyEnable) is enabled through config option, the hardware WLT hints will be ignored. On detecting workload type, framework takes predefined action with values in config file.

# Pre-requisite

|Workload Type (WLT) |Description |Internal states |EPP/EPB|
| :---:              | :---:      |:---:           |:---:  |
|Idle |Very low system usage and low power consumption |DEEP_MODE|PS/BAT |
|BL(Battery Life)|Continuous light system usage with low power consumption |NORM_MODE, RESP_MODE   |PS/BAT (platform optimal value)|
|Sustained |Continuous heavy tasks without idleness|MDRT_MODE INIT_MODE |Perf/AC (platform optimal value)|
|Bursty |Heavy short tasks with idleness in between |PERF_MODE |Perf/AC|

PS - Power saver; Perf - performance; BAT - Battery bias; AC - AC bias.

# Enabling and leveraging wlt proxy hints for dynamic energy optimization.
    * Software based WLT Proxy hint overwrites hardware based WLT Hint. To enable proxy hints, both WLT Hint and WLTProxy has to be enabled.
        ** WLTHintEnable set to Yes
        <WLTHintEnable>1</WLTHintEnable>
        ** WLTProxyEnable set to Yes
        <WLTProxyEnable>1</WLTProxyEnable>

    * WLT Proxy hints are calculated and dynamic energy optimizations are applied only in balanced power profile. Set to auto or force on.
        ** BalancedDef set to AUTO
        <BalancedDef>0<BalancedDef>

# Value add
    with Dynamic EPP [based on workload] enabled on Core Ultra Gen 1 [Meteor Lake H], HP Baymax14W, CDB, PV SKU8W - N15479-021, in Balanced power profile, 10% performance improvement observed on Crossmark and Speedometer benchmarks.

## Known issues
    * Performance may suffer on some use cases.
        ** As algorithm currently takes average of all p-cores and e-cores utilization to identify workload type, solution has limitation of identifying single threaded workloads and memory related workloads. Benchmarks like Geekbench ST, WebXprt, Stream may not show improvement compared to Geekbench MT, Speedometer and Crossmark.

# Workload detection algorithm - pseudo code
   * System CPU utilization thresholds and conditions are predefined and mapped to workload type.
   * Operating CPU frequency, Spike count rate, state stay time counter and operating frequency-voltage points makes up state switch conditions.
   * Enabling WLT proxy through config file calls WLT proxy handler.
   * On WLT Proxy handler,
        ** CPU load retrieved from system through perf MSR registers (system snapshot)
        ** State machine, switch state when predefined conditions are met
        ** Apply state actions
        ** Calculate/set new timer interval
   * When timer expires, WLT Proxy handler called again

# WLT proxy states
|       | Init  | Perf | Mod4e | Mod3e | Mod2e | Resp | Normal | deep |
| :---: |:---:  |:---: | :---: | :---: | :---: | :---: | :---: | :---: |
| init  |     x | [1 cpu].lo < 10 utilization| -| -| -| -| -| - |
| Perf  | [all cpu].lo > 10 utilization| x| -| C0 max < 10%| -| sum_c0 util < 20% && sma avg < 70 %| -| - |
| MOD4E | -     | C0_max > 90%| x| -| -| worst_stall < 70%| sma_avg1 < 25 AND sma_avg2 < 25 AND sum_c0 < 50%| - |
| MOD3E | -     | C0_max > 90%| sma_avg1 > 25 AND sma_avg2 > 20| x| sma_avg1 b.w 4 and 25 AND sma_avg2 b/w 4 and 25| worst_stall < 70%| sma_avg1 < 4 AND sma_avg2 < 2 AND sma_avg3 < 2| - |
| MOD2E | -     | -     | -| C0_max > 90% OR sma_avg1 > 25  AND sma_avg2 > 15| x| sorst_stall < 70%| sma_avg1 b/w 4 and 25 AND sma_avg2 < 25 countdown and switch| - |
| Resp  | -     | C0_max > 70% && sma_avg1 > 40%| -| worst stall > 70%| -| x| -| - |
| Normal| -     | -     | -| -| C0_max > 50% OR sma_avg1 > 40| worst stall < 70%| x| C0_max < 10% AND C0_2ndMax < 1% OR sma_avg1 < 2%; countdown and switch |
| Deep  | -     | -     | -| -| -| worst_stall < 70%| C0_max > 35%| x |

x – invalid/same state; - not allowed state

Variables used in state switch condition:
*    Multithreaded workload: all applicable CPUs for state utilized more that 10%. Not multithread workload: at least one CPU is utilized < 10%.
*    CPU.L0= 100 * perf_stats[t].mperf_diff /  perf_stats[t].tsc_diff
        * Also following values will be calculated based on L0. C0_max , Min_load[C0.min]; max_load, max_2nd, max_3rd
*    sum_c0 = grp.c0_max + grp.c0_2nd_max + grp.c0_3rd_max
*    sma - simple moving average [tracks 3 max utilizations]
*    worst stall - perf_stats[t].pperf_diff /  perf_stats[t].aperf_diff. cpu in wait due to memory of other dependency.

# Files & functionality

## wlt_proxy.c
   * wlt proxy detection interface file
   * handles wlt_proxy enable/disable; entry/exit, timer expiry handler
## state_machine.c
   * handle current state
   * determine state change
## spike_mgmt.c
   * counts CPU utilization spike counts in given period
   * handles bursty workload type detection entry and exit
## state_util.c
   * retrieval  of pref, HFM and SMA
   * calculations for state switch
## state_manager.c
   * state definition & management
   * state initialization/deinitialization
   * mapping state to workload type
   * polling frequency calculations
