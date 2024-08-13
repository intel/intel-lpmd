
WLT [work load type] proxy can be enabled/disabled through config option. Uses CPU utilization to detect workload type. Applies optimal EPP values tailered for specific workload. 

# Workload Types

 * Idle 
    - system performs no tasks for long period of time. Power conusmption very low.
 * Battery Life 
    - system performs task continously [VPB]. power consumption low.
 * Sustained 
    - continously perform task with high power consumption. PL1/PL2 exhausted.
 * Bursty 
    - Mixed idleness and bursty cpu utilization. PL1 exhausted.

PL - Power level. PL1

# Assumptions: 
  * CPU C0 is always on.

# Workload detection Algorithm

 ## Utilization thresholds.
 ## state machine
    INIT, PREF, RESP, 
 ## spike detection
    spike burst refers to continous spikes in a series of back to back samples. 
 ## burst count calculation
 ## pref calculation
    
 ## knobs 
