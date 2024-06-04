#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "../additional_common.h"

int main() {

   //system("cat /etc/sudoers | head -n 1" );
	
   printf("drop! \n");
   if(_drop_privilege()) {
      printf("error - drop! \n");
   }
   
   system("cat /etc/sudoers | head -n 1");
   
   printf("raise! \n");
   if(_raise_privilege()) {
      printf("error - raise! \n");
   }
   
   system("cat /etc/sudoers | head -n 1");
   
   /*printf("drop! \n");
   _drop_privilege();  
   
   system("cat /etc/sudoers | head -n 1");
   */
   return 0;
   
}
