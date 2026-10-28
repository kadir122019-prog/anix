#include <anix/string.h>
size_t kstrlen(const char *s){size_t n=0;if(s)while(s[n])++n;return n;}
void *kmemset(void *d,int v,size_t n){unsigned char *p=d;while(n--)*p++=(unsigned char)v;return d;}
void *kmemcpy(void *d,const void *s,size_t n){unsigned char *x=d;const unsigned char *y=s;while(n--)*x++=*y++;return d;}
int kmemcmp(const void *a,const void *b,size_t n){const unsigned char*x=a,*y=b;while(n--){if(*x!=*y)return *x<*y?-1:1;++x;++y;}return 0;}
void *memset(void *d,int v,size_t n){return kmemset(d,v,n);}
void *memcpy(void *d,const void *s,size_t n){return kmemcpy(d,s,n);}
