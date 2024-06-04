#include <stdio.h>
#include <time.h>
#include "../weights_common.h"

int main() {

   //printf("Hello, World!");
   printf("test 1: first call \n");
   //time_t t1 = time(0);
   is_ac_powered_power_supply_status();
   //time_t t2 = time(0);
   //printf("************************latency in seconds =  %d \n", ((t2 - t1) * 1000 * 1000));
   
   printf("test 2: consecutive call  - should be faster \n");
   is_ac_powered_power_supply_status();
   printf("######################## \n");
   
   printf("test 3: status check \n");
   printf ("is_available_power_supply_status = %d \n", PSS_is_available());   
   printf("######################## \n");
   
   printf("test 3: reset and try \n");
   PSS_deinit();
   printf ("is_available_power_supply_status = %d \n", PSS_is_available());
   PSS_init();
   is_ac_powered_power_supply_status();   
   printf("######################## \n");
   
   return 0;
   
}
