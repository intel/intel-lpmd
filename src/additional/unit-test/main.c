#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "../additional_common.h"

int main() {

   system("cat /etc/sudoers | head -n 1" );
	
   printf("drop! \n");
   _drop_privilege();
   
   system("cat /etc/sudoers | head -n 1");
   
   printf("raise! \n");
   _raise_privilege();
   
   system("cat /etc/sudoers | head -n 1");
   
   printf("drop! \n");
   _drop_privilege();  
   
   system("cat /etc/sudoers | head -n 1");
   
   return 0;
   
}
