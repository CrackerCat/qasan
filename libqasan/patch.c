/*******************************************************************************
Copyright (c) 2019-2020, Andrea Fioraldi


Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*******************************************************************************/

#include "libqasan.h"
#include <sys/mman.h>

#ifdef __x86_64__

uint8_t* __libqasan_patch_jump(uint8_t* addr, uint8_t* dest) {

  // mov rax, dest
  addr[0] = 0x48;
  addr[1] = 0xb8;
  *(uint8_t**)&addr[2] = dest;
  
  // jmp rax
  addr[10] = 0xff;
  addr[11] = 0xe0;
  
  return &addr[12];

}

#elif __i386__

uint8_t* __libqasan_patch_jump(uint8_t* addr, uint8_t* dest) {

  // mov eax, dest
  addr[0] = 0xb8;
  *(uint8_t**)&addr[1] = dest;
  
  // jmp eax
  addr[5] = 0xff;
  addr[6] = 0xe0;
  
  return &addr[7];

}

#elif __arm__

// in ARM, r12 is a scratch register used by the linker to jump,
// so let's use it in our stub

uint8_t* __libqasan_patch_jump(uint8_t* addr, uint8_t* dest) {

  // ldr r12, OFF
  addr[0] = 0x0;
  addr[1] = 0xc0;
  addr[2] = 0x9f;
  addr[3] = 0xe5;
  
  // add pc, pc, r12
  addr[4] = 0xc;
  addr[5] = 0xf0;
  addr[6] = 0x8f;
  addr[7] = 0xe0;
  
  // OFF: .word dest
  *(uint32_t*)&addr[8] = (uint32_t)dest;
  
  return &addr[12];

}

#elif __aarch64__

// in ARM64, x16 is a scratch register used by the linker to jump,
// so let's use it in our stub

uint8_t* __libqasan_patch_jump(uint8_t* addr, uint8_t* dest) {

  // ldr x16, OFF
  addr[0] = 0x50;
  addr[1] = 0x0;
  addr[2] = 0x0;
  addr[3] = 0x58;
  
  // br x16
  addr[4] = 0x0;
  addr[5] = 0x2;
  addr[6] = 0x1f;
  addr[7] = 0xd6;
  
  // OFF: .dword dest
  *(uint64_t*)&addr[8] = (uint64_t)dest;
  
  return &addr[16];

}

#else

#define CANNOT_HOTPATCH

#endif

#ifdef CANNOT_HOTPATCH

void __libqasan_hotpatch(void) {}

#else

#include "pmparser.h"

static void* libc_start, *libc_end;
int libc_perms;

static void find_libc(void) {

  procmaps_iterator* maps = pmparser_parse(-1);
  procmaps_struct*   maps_tmp = NULL;

  while ((maps_tmp = pmparser_next(maps)) != NULL) {
  
    if (maps_tmp->is_x && (__libqasan_strstr(maps_tmp->pathname, "/libc.so") ||
        __libqasan_strstr(maps_tmp->pathname, "/libc-"))) {
    
      libc_start = maps_tmp->addr_start;
      libc_end = maps_tmp->addr_end;
      
      libc_perms = PROT_EXEC;
      if (maps_tmp->is_w) libc_perms |= PROT_WRITE;
      if (maps_tmp->is_r) libc_perms |= PROT_READ;
      
      break;

    }

  }

  pmparser_free(maps);
  
}

/* Why this shit? https://twitter.com/andreafioraldi/status/1227635146452541441
   Unfortunatly, symbol override with LD_PRELOAD is not enough to prevent libc
   code to call this optimized XMM-based routines.
   We patch them at runtime to call our unoptimized version of the same routine. 
*/

void __libqasan_hotpatch(void) {

  find_libc();

  if (!libc_start) return;

  if (mprotect(libc_start, libc_end - libc_start,
               PROT_READ | PROT_WRITE | PROT_EXEC) < 0)
    return;

  void* libc = dlopen("libc.so.6", RTLD_LAZY);
  
#define HOTPATCH(fn) \
  void* p_## fn = dlsym(libc, # fn); \
  if (p_## fn) __libqasan_patch_jump(p_## fn, (uint8_t*)&(fn)); \
  
  HOTPATCH(memcmp)
  HOTPATCH(memchr)
  HOTPATCH(memrchr)
  HOTPATCH(memmem)
  HOTPATCH(bzero)
  HOTPATCH(explicit_bzero)
  HOTPATCH(bcmp)
  
  HOTPATCH(strchr)
  HOTPATCH(strrchr)
  HOTPATCH(strcasecmp)
  HOTPATCH(strncasecmp)
  HOTPATCH(strcat)
  HOTPATCH(strcmp)
  HOTPATCH(strncmp)
  HOTPATCH(strcpy)
  HOTPATCH(strncpy)
  HOTPATCH(stpcpy)
  HOTPATCH(strdup)
  HOTPATCH(strlen)
  HOTPATCH(strnlen)
  HOTPATCH(strstr)
  HOTPATCH(strcasestr)

#undef HOTPATCH

  mprotect(libc_start, libc_end - libc_start, libc_perms);

}

#endif
