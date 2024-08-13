

WLT proxy uses CPU utilization to detect workload type, can be enabled/disabled through config option. Applies optimal EPP values tailered for specific workload.

# How Algorithm works.

 * Defined Utilization thresholds.
 * state machine
    INIT, PREF, RESP, 
 * spike detection
 * burst count calculation
 * pref calculation
 * knobs 
