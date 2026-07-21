/* Compact MD5 (public domain style) */
#include "md5.h"
#include <string.h>

typedef struct { uint32_t s[4]; uint64_t bits; uint8_t buf[64]; } MD5_CTX;
static uint32_t F(uint32_t x,uint32_t y,uint32_t z){return (x&y)|(~x&z);}
static uint32_t G(uint32_t x,uint32_t y,uint32_t z){return (x&z)|(y&~z);}
static uint32_t H(uint32_t x,uint32_t y,uint32_t z){return x^y^z;}
static uint32_t I(uint32_t x,uint32_t y,uint32_t z){return y^(x|~z);}
static uint32_t rot(uint32_t x,int n){return (x<<n)|(x>>(32-n));}
static void step(uint32_t *a,uint32_t b,uint32_t c,uint32_t d,uint32_t x,uint32_t t,int s,
  uint32_t (*f)(uint32_t,uint32_t,uint32_t)){
  *a = b + rot(*a + f(b,c,d) + x + t, s);
}
static void body(MD5_CTX *ctx, const uint8_t *p){
  uint32_t a=ctx->s[0],b=ctx->s[1],c=ctx->s[2],d=ctx->s[3],x[16];
  for(int i=0;i<16;i++) x[i]= (uint32_t)p[i*4] | ((uint32_t)p[i*4+1]<<8) |
    ((uint32_t)p[i*4+2]<<16) | ((uint32_t)p[i*4+3]<<24);
  #define R1(a,b,c,d,k,s,t) step(&a,b,c,d,x[k],t,s,F)
  #define R2(a,b,c,d,k,s,t) step(&a,b,c,d,x[k],t,s,G)
  #define R3(a,b,c,d,k,s,t) step(&a,b,c,d,x[k],t,s,H)
  #define R4(a,b,c,d,k,s,t) step(&a,b,c,d,x[k],t,s,I)
  R1(a,b,c,d,0,7,0xd76aa478); R1(d,a,b,c,1,12,0xe8c7b756); R1(c,d,a,b,2,17,0x242070db); R1(b,c,d,a,3,22,0xc1bdceee);
  R1(a,b,c,d,4,7,0xf57c0faf); R1(d,a,b,c,5,12,0x4787c62a); R1(c,d,a,b,6,17,0xa8304613); R1(b,c,d,a,7,22,0xfd469501);
  R1(a,b,c,d,8,7,0x698098d8); R1(d,a,b,c,9,12,0x8b44f7af); R1(c,d,a,b,10,17,0xffff5bb1); R1(b,c,d,a,11,22,0x895cd7be);
  R1(a,b,c,d,12,7,0x6b901122); R1(d,a,b,c,13,12,0xfd987193); R1(c,d,a,b,14,17,0xa679438e); R1(b,c,d,a,15,22,0x49b40821);
  R2(a,b,c,d,1,5,0xf61e2562); R2(d,a,b,c,6,9,0xc040b340); R2(c,d,a,b,11,14,0x265e5a51); R2(b,c,d,a,0,20,0xe9b6c7aa);
  R2(a,b,c,d,5,5,0xd62f105d); R2(d,a,b,c,10,9,0x02441453); R2(c,d,a,b,15,14,0xd8a1e681); R2(b,c,d,a,4,20,0xe7d3fbc8);
  R2(a,b,c,d,9,5,0x21e1cde6); R2(d,a,b,c,14,9,0xc33707d6); R2(c,d,a,b,3,14,0xf4d50d87); R2(b,c,d,a,8,20,0x455a14ed);
  R2(a,b,c,d,13,5,0xa9e3e905); R2(d,a,b,c,2,9,0xfcefa3f8); R2(c,d,a,b,7,14,0x676f02d9); R2(b,c,d,a,12,20,0x8d2a4c8a);
  R3(a,b,c,d,5,4,0xfffa3942); R3(d,a,b,c,8,11,0x8771f681); R3(c,d,a,b,11,16,0x6d9d6122); R3(b,c,d,a,14,23,0xfde5380c);
  R3(a,b,c,d,1,4,0xa4beea44); R3(d,a,b,c,4,11,0x4bdecfa9); R3(c,d,a,b,7,16,0xf6bb4b60); R3(b,c,d,a,10,23,0xbebfbc70);
  R3(a,b,c,d,13,4,0x289b7ec6); R3(d,a,b,c,0,11,0xeaa127fa); R3(c,d,a,b,3,16,0xd4ef3085); R3(b,c,d,a,6,23,0x04881d05);
  R3(a,b,c,d,9,4,0xd9d4d039); R3(d,a,b,c,12,11,0xe6db99e5); R3(c,d,a,b,15,16,0x1fa27cf8); R3(b,c,d,a,2,23,0xc4ac5665);
  R4(a,b,c,d,0,6,0xf4292244); R4(d,a,b,c,7,10,0x432aff97); R4(c,d,a,b,14,15,0xab9423a7); R4(b,c,d,a,5,21,0xfc93a039);
  R4(a,b,c,d,12,6,0x655b59c3); R4(d,a,b,c,3,10,0x8f0ccc92); R4(c,d,a,b,10,15,0xffeff47d); R4(b,c,d,a,1,21,0x85845dd1);
  R4(a,b,c,d,8,6,0x6fa87e4f); R4(d,a,b,c,15,10,0xfe2ce6e0); R4(c,d,a,b,6,15,0xa3014314); R4(b,c,d,a,13,21,0x4e0811a1);
  R4(a,b,c,d,4,6,0xf7537e82); R4(d,a,b,c,11,10,0xbd3af235); R4(c,d,a,b,2,15,0x2ad7d2bb); R4(b,c,d,a,9,21,0xeb86d391);
  ctx->s[0]+=a; ctx->s[1]+=b; ctx->s[2]+=c; ctx->s[3]+=d;
}
static void md5_init(MD5_CTX *c){c->s[0]=0x67452301;c->s[1]=0xefcdab89;c->s[2]=0x98badcfe;c->s[3]=0x10325476;c->bits=0;}
static void md5_update(MD5_CTX *c,const uint8_t *data,size_t len){
  size_t i = (size_t)((c->bits>>3)&63); c->bits += (uint64_t)len<<3;
  size_t part = 64 - i;
  size_t idx=0;
  if(len >= part){ memcpy(c->buf+i,data,part); body(c,c->buf); idx=part;
    for(; idx+63<len; idx+=64) body(c,data+idx); i=0; }
  memcpy(c->buf+i, data+idx, len-idx);
}
static void md5_final(MD5_CTX *c,uint8_t out[16]){
  size_t i=(size_t)((c->bits>>3)&63); c->buf[i++]=0x80;
  if(i>56){ while(i<64) c->buf[i++]=0; body(c,c->buf); i=0; }
  while(i<56) c->buf[i++]=0;
  uint64_t b=c->bits;
  for(int j=0;j<8;j++) c->buf[56+j]=(uint8_t)(b>>(8*j));
  body(c,c->buf);
  for(int j=0;j<4;j++){ out[j*4]=(uint8_t)c->s[j]; out[j*4+1]=(uint8_t)(c->s[j]>>8);
    out[j*4+2]=(uint8_t)(c->s[j]>>16); out[j*4+3]=(uint8_t)(c->s[j]>>24); }
}
void md5(const uint8_t *data, size_t len, uint8_t out[16]){
  MD5_CTX c; md5_init(&c); md5_update(&c,data,len); md5_final(&c,out);
}
