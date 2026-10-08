/* Replays linkPeersJson's exact string-building against a canary, both the
   old (accumulate snprintf's return) and new (clamped append) way. */
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#define LINK_MAX_PEERS 8
struct Peer { char id[9], name[24], session[16], ips[16]; unsigned photos; unsigned char flags; unsigned seen; bool used; };
static Peer peers[LINK_MAX_PEERS];
static const char* group="wuw"; static const char* selfId="A1B2C3D4"; static bool up=true;
static char pullStat[72]="idle";
static const char* linkPullStatus(){ return pullStat; }

/* ── the version that shipped in my first draft ── */
static size_t oldWay(char* out,size_t cap){
  size_t w=0;
  w+=snprintf(out+w,cap-w,"{\"ok\":true,\"up\":%s,\"group\":\"%s\",\"me\":\"%s\",\"peers\":[",
              up?"true":"false",group,selfId);
  for(int i=0;i<LINK_MAX_PEERS && w<cap-96;i++){
    if(!peers[i].used) continue;
    w+=snprintf(out+w,cap-w,
      "%s{\"id\":\"%s\",\"name\":\"%s\",\"ip\":\"%s\",\"ses\":\"%s\",\"photos\":%u,\"sd\":%s,\"age\":%u}",
      i?",":"",peers[i].id,peers[i].name,peers[i].ips,peers[i].session,
      peers[i].photos,(peers[i].flags&1)?"true":"false",peers[i].seen);
  }
  w+=snprintf(out+w,cap-w,"],\"pull\":\"%s\"}",linkPullStatus());
  return w;
}

/* ── the fix ── */
static bool jappend(char* out,size_t cap,size_t& w,const char* src){
  size_t n=strlen(src);
  if(w+n>=cap) return false;
  memcpy(out+w,src,n); w+=n; out[w]=0; return true;
}
static size_t newWay(char* out,size_t cap){
  if(!out||cap<128) return 0;
  size_t w=0; out[0]=0;
  char head[128];
  snprintf(head,sizeof(head),"{\"ok\":true,\"up\":%s,\"group\":\"%s\",\"me\":\"%s\",\"peers\":[",
           up?"true":"false",group,selfId);
  jappend(out,cap,w,head);
  char tail[128]; snprintf(tail,sizeof(tail),"],\"pull\":\"%s\"}",linkPullStatus());
  const size_t reserve=strlen(tail);
  int shown=0;
  for(int i=0;i<LINK_MAX_PEERS;i++){
    if(!peers[i].used) continue;
    char one[192];
    snprintf(one,sizeof(one),
      "%s{\"id\":\"%s\",\"name\":\"%s\",\"ip\":\"%s\",\"ses\":\"%s\",\"photos\":%u,\"sd\":%s,\"age\":%u}",
      shown?",":"",peers[i].id,peers[i].name,peers[i].ips,peers[i].session,
      peers[i].photos,(peers[i].flags&1)?"true":"false",peers[i].seen);
    if(w+strlen(one)+reserve>=cap) break;
    jappend(out,cap,w,one); shown++;
  }
  jappend(out,cap,w,tail);
  return w;
}

/* crude but sufficient: braces/brackets balance and it ends correctly */
static bool looksValid(const char* j){
  int b=0,k=0; for(const char* p=j;*p;p++){ if(*p=='{')b++; else if(*p=='}')b--;
    else if(*p=='[')k++; else if(*p==']')k--; if(b<0||k<0) return false; }
  size_t L=strlen(j);
  return b==0&&k==0&&L>2&&j[L-1]=='}';
}

struct Guarded { char buf[1024]; char canary[64]; };

int main(){
  /* worst case the wire can produce: full table, longest legal fields */
  for(int i=0;i<LINK_MAX_PEERS;i++){
    peers[i].used=true;
    snprintf(peers[i].id,sizeof(peers[i].id),"DEADBEE%d",i);
    memset(peers[i].name,'W',sizeof(peers[i].name)-1); peers[i].name[sizeof(peers[i].name)-1]=0;
    memset(peers[i].session,'S',sizeof(peers[i].session)-1); peers[i].session[sizeof(peers[i].session)-1]=0;
    snprintf(peers[i].ips,sizeof(peers[i].ips),"192.168.100.%d",100+i);
    peers[i].photos=999999; peers[i].flags=1; peers[i].seen=8;
  }
  snprintf(pullStat,sizeof(pullStat),"%s","saved LNK_WWWWWWWWWWWWWWW_0042.jpg 1998KB");

  int fails=0;
  for(int which=0;which<2;which++){
    Guarded g; memset(&g,0,sizeof(g));
    memset(g.canary,0x5A,sizeof(g.canary));
    size_t n = which? newWay(g.buf,sizeof(g.buf)) : oldWay(g.buf,sizeof(g.buf));
    bool smashed=false;
    for(size_t i=0;i<sizeof(g.canary);i++) if((unsigned char)g.canary[i]!=0x5A) smashed=true;
    bool over = n>=sizeof(g.buf);
    printf("%-8s returned=%4zu  cap=%zu  canary=%-9s json=%s\n",
           which?"FIXED":"ORIGINAL", n, sizeof(g.buf),
           smashed?"SMASHED":"intact", looksValid(g.buf)?"valid":"INVALID");
    if(over) printf("           ^ reported length %zu EXCEEDS the buffer -> next write uses cap-w = %zu\n",
                    n, (size_t)(sizeof(g.buf)-n));
    if(which==1 && (smashed||!looksValid(g.buf))) fails++;
  }

  /* the fix must also hold at every buffer size, not just 1024 */
  for(size_t cap=128; cap<=1600; cap+=7){
    char* b=(char*)malloc(cap+32); memset(b+cap,0x5A,32);
    size_t n=newWay(b,cap);
    for(size_t i=0;i<32;i++) if((unsigned char)b[cap+i]!=0x5A){ printf("SMASH at cap=%zu\n",cap); fails++; break; }
    if(n>=cap){ printf("OVERRUN report at cap=%zu (%zu)\n",cap,n); fails++; }
    if(!looksValid(b)){ printf("INVALID JSON at cap=%zu: %s\n",cap,b); fails++; }
    free(b);
  }
  printf("\nsweep 128..1600 bytes (fixed): %s\n", fails?"FAILURES":"clean, valid JSON at every size");

  /* Is the memory smash actually reachable, or only the truncation? Sweep the
     ORIGINAL over buffer sizes and name lengths and count real canary hits. */
  int smashes=0, invalids=0, probes=0; size_t worst=0; size_t badCap=0; int badLen=0;
  for(int nameLen=4; nameLen<23; nameLen++){
    for(int i=0;i<LINK_MAX_PEERS;i++){
      memset(peers[i].name,'W',nameLen); peers[i].name[nameLen]=0;
    }
    for(size_t cap=200; cap<=1400; cap++){
      probes++;
      char* b=(char*)malloc(cap+128); memset(b,0,cap+128); memset(b+cap,0x5A,128);
      size_t n=oldWay(b,cap);
      bool sm=false; size_t past=0;
      for(size_t i=0;i<128;i++) if((unsigned char)b[cap+i]!=0x5A){ sm=true; past=i+1; }
      if(sm){ smashes++; if(past>worst){worst=past; badCap=cap; badLen=nameLen;} }
      if(!looksValid(b)) invalids++;
      free(b);
    }
  }
  printf("ORIGINAL swept over %d shapes: %d wrote past the buffer, %d produced invalid JSON\n",
         probes, smashes, invalids);
  if(smashes) printf("  worst overrun %zu bytes past the end (cap=%zu, name length %d)\n",
                     worst, badCap, badLen);
  return fails?1:0;
}
