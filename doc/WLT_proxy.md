
WLT [work load type] proxy hints uses predefined CPU utilization thresholds to detect WLT. Proxy hints can be enabled/disabled through config option. On detecting work load type, proxy program request lpmd framework to take predefined action with values in config file.

# Workload Types
 * Idle
    - system performs no tasks. Power conusmption very low.
 * Battery Life 
    - system performs task continously with low power consumption.
 * Sustained 
    - continously perform task with high power consumption. PL1/PL2 exhausted. PL - Power level.
 * Bursty 
    - Mixed idleness and Spikes in cpu utilization. PL1 exhausted.

# Pre-requiste

 |Workload Type [WLT]                               |Internal states        |EPP/EPB                        |
 | :---:                                            |:---:                  |:---:                          | 
 |IDLE [almost no task/cpu utilization]             |DEEP_MODE              |PS/BAT [255/15]                |
 |BL[continuous - light task]                       |NORM_MODE, RESP_MODE   |PS/BAT [platform optimal vale] |
 |Sustained[Continuous heavy task without IDLEness] |MDRT_MODE INIT_MODE    |Perf/AC [platform optimal vale]|
 |Bursty[heavy short tasks with IDLEness in between]|PERF_MODE              |Perf/AC [64,4]                 |

# Workload detection algorithm - pseudo code
   * CPU utilization thresholds predefined for each state [low, moderate, perf]. 
        * CPU load retrived from system through perf MSR (registers) [aperf [average], mperf [maximum], pperf, tsc] readings
   * state machine, based on threshold, buckets system load into a state
   * normalization using spike and burst count detection - continous spikes in a series of back to back samples vs brust now and then.
   * Map state to work load type [WLT].        
   * knobs are external user actions that impact WLT proxy calculations.
        * For example Power supply change - impacts the EPB values to set.
   * calculate next poll time and request framework to apply state actions.

# Proxy Internal States.
|       | Init  | Perf | Mod4e | Mod3e | Mod2e | Resp | Normal | deep | 
| :---: |:---:  |:---: | :---: | :---: | :---: | :---: | :---: | :---: |
| init  |     x | [1 cpu].lo < 10 utliization| -| -| -| -| -| - |
| Perf  | [all cpu].lo > 10 utliization| x| -| C0 max < 10%| -| sum_c0 util < 20% && sma avg < 70 %| -| - |
| MOD4E | -     | C0_max > 90%| x| -| -| Worst_stall < 70%| Sma_avg1 < 25 AND sma_avg2 < 25 AND sum_c0 < 50%| - |
| MOD3E | -     | C0_max > 90%| Sma_avg1 > 25 AND sma_avg2 > 20| x| Sma_avg1 b.w 4 and 25 AND sma_avg2 b/w 4 and 25| Worst_stall < 70%| Sma_avg1 < 4 AND sma_avg2 < 2 AND sma_avg3 < 2| - |
| MOD2E | -     | -     | -| C0_max > 90% OR sma_avg1 > 25  AND sma_avg2 > 15| x| Worst_stall < 70%| Sma_avg1 b/w 4 and 25 AND sma_avg2 < 25 Countdown and switch| - |
| Resp  | -     | C0_max > 70% && sma_avg1 > 40%| -| Worst stall > 70%| -| x| -| - |
| Normal| -     | -     | -| -| C0_max > 50% OR sma_avg1 > 40| Worst stall < 70%| x| C0_max < 10% AND C0_2ndMax < 1% OR sma_avg1 < 2%; Countdown and switch |
| Deep  | -     | -     | -| -| -| Worst_stall < 70%| C0_max > 35%| x |
x – invalid/same state; - not allowed state

Description:
*    All CPUs applicable to all states.
*    Not multithread worked – at least one CPU is utilized < 10%
*    Multithreaded workload – all applicable CPUs for state utilized more that 10%
*    sum_c0 = grp.c0_max + grp.c0_2nd_max + grp.c0_3rd_max
*    sma - simple moving average [tracks 3 max utilizations]
*    CPU.L0= 100 *perf_stats[t].mperf_diff /  perf_stats[t].tsc_diff
        * C0_max , Min_load[C0.min]; max_load, max_2nd, max_3rd
    
*    Stall – means stop making progress. [due to memory or other dependency]
*    Worst stall - perf_stats[t].pperf_diff /  perf_stats[t].aperf_diff

# Files & functionality:

## wlt proxy entry
   handles wlt_proxy enable/disable; entry/exit
## state machine
   handle current state 
   determine state change
## spike management
   handle bursty work load type detection.
## state_util
   define states
   retrival of cpu utilization & prefs
   calculations and mapping state to workload type
   check knobs
## state_manager
   state information definition & management
   state initialization
   polling frequency calculations.
   CPU topology management - turned off
   power clamp management - turned off
   IRQ rebalance - turned off
   idle inject - turned off
   IRQ affinity - turned off
## Power supply knob
   detect battery presense and charging status.
