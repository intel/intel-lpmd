
WLT [work load type] proxy hints uses predefined CPU utilization thresholds to detect WLT. Proxy hints can be enabled/disabled through config option. On detecting work load type, proxy program request lpmd framework to take predefined action with values in config file.

# Workload Types
 * Idle
    - system performs no tasks for long period of time. Power conusmption very low.
 * Battery Life 
    - system performs task continously [example: VPB]. power consumption low.
 * Sustained 
    - continously perform task with high power consumption. PL1/PL2 exhausted.
 * Bursty 
    - Mixed idleness and bursty cpu utilization. PL1 exhausted.

PL - Power level.

# Assumptions: 
  * CPU C0 is always on.

# Workload detection algorithm - pseudo code
   * CPU utilization thresholds predefined for each state [low, moderate, perf]. 
        <explain more with math functions.>
        init -> multi threads detected -> sustained WLT -> 
        moderate -> battery life WLT -> 
        performance state ->
        
        
   * CPU load retrived from system through perf MSR (registers) [aperf [average], mperf [maximum], pperf, tsc] readings
   * state machine, based on threshold, buckets system load into a state
   * normalization using spike and burst count detection - continous spikes in a series of back to back samples vs brust now and then.
   * Map state to work load type [WLT].
        <explain more on mapping>
   * knobs are external user actions that impact WLT proxy calculations.
      * For example Power supply change - impacts the EPB values to set.
   * calculate next poll time and request framework to apply state actions.

   
# Files & functionality:

## wlt proxy entry
   handles wlt_proxy enable/disable; entry/exit
## state machine
   handle current state 
   determine state change
## spike management
   handle bursty work load type detection.
## util
   define states
   retrival of cpu utilization & prefs
   calculations and mapping state to workload type
   check knobs
## cpu_group
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
