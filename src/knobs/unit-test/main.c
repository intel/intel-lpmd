#include <stdio.h>
#include <time.h>
#include "../weights_common.h"

int main() {
	
	printf("test 1: first call \n");
	printf ("PPS_is_performance_governer = %d \n",PPS_is_performance_governer());
	
	
   printf("test 1: first call \n");
   printf ("is_available_hardware wlt hint _status = %d \n", HWS_is_available());
   
   printf("test 2: reset and try \n");
   HWS_deinit();
   printf ("is_available_hardware wlt hint _status = %d \n", HWS_is_available());
   printf("######################## \n");
   
   printf("test 3: init gain \n");
   HWS_deinit();
   HWS_init();
   printf("######################## \n");
   
   printf("test 4: status check \n");
   printf ("is_available_hardware wlt hint _status = %d \n", HWS_is_available()); 
   printf("######################## \n");
   
   

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
