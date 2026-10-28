#include <anix/string.h>
#include <assert.h>
#include <stdio.h>
int main(void){char a[8];assert(kstrlen("anix")==4);kmemset(a,'x',4);assert(a[0]=='x'&&a[3]=='x');kmemcpy(a,"boot",5);assert(kmemcmp(a,"boot",5)==0);puts("[pass] kernel string primitives");return 0;}
