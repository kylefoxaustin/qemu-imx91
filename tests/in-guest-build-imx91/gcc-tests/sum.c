// EXPECT: SUM=5050
#include <stdio.h>
int main(void){ int s=0; for (int i=1;i<=100;i++) s+=i; printf("SUM=%d\n", s); return 0; }
