
WLT [work load type] proxy hints uses CPU utilization to detect WLT. Can be enabled/disabled through config option. On detecting WLT, program request lpmd framework to take action predefined in config file.

# Workload Types
 * Idle
    - system performs no tasks for long period of time. Power conusmption very low.
 * Battery Life 
    - system performs task continously [VPB]. power consumption low.
 * Sustained 
    - continously perform task with high power consumption. PL1/PL2 exhausted.
 * Bursty 
    - Mixed idleness and bursty cpu utilization. PL1 exhausted.

PL - Power level.

# Assumptions: 
  * CPU C0 is always on.

# Workload detection Algorithm

 ## CPU utilization thresholds predefined in include/wlt_proxy_common.h.
 ## state machine based on threshold buckets into a state
    INIT, PREF, RESP, 
 ## normalization
   spike and burst count detection -   spike burst refers to continous spikes in a series of back to back samples.  
 ## pref calculation
   
 ## state mapped to work load type hint.

 ## knobs 
   knobs are external user actions that impact WLT proxy calculations.


wlt_proxy_entry
   handles wlt_proxy enable/disable entry/exit
state_machine
   define and handle internal states
spike management
   handle bursty work load type detection.
