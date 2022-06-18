/*
        Copyright 2019 devMiyax(smiyaxdev@gmail.com)

This file is part of YabaSanshiro.

        YabaSanshiro is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

YabaSanshiro is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

        You should have received a copy of the GNU General Public License
along with YabaSanshiro; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/

/*! \file sh2cache.c
\brief SH2 internal cache operations FIL0016332.PDF section 8
*/

#ifdef PSP
#include <stdint.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <ctype.h>

#include "memory.h"
#include "yabause.h"
#include "sh2cache.h"
#include "sh2core.h"

#include "debug.h"

#define AREA_MASK (0xE0000000)
#define TAG_MASK (0x1FFFFC00)
#define ENTRY_MASK (0x000003F0)
#define ENTRY_SHIFT (4)
#define LINE_MASK (0x0000000F)

#define CACHE_USE ((0x00) << 29)
#define CACHE_THROUGH ((0x01) << 29)
#define CACHE_PURGE ((0x02) << 29)
#define CACHE_ADDRES_ARRAY ((0x03) << 29)
#define CACHE_DATA_ARRAY ((0x06) << 29)
#define CACHE_IO ((0x07) << 29)

#define MAX_CACHE_MISS_CYCLE (128)

FILE *cache_f = NULL;
//#define COHERENCY_CHECK

void cache_clear(cache_enty *ca)
{
  int entry = 0;
  ca->enable = 0;

  for (entry = 0; entry < 64; entry++)
  {
    int way = 0;
    ca->lru[entry] = 0;

    for (way = 0; way < 4; way++)
    {
      int i = 0;
      ca->way[entry].tag[way] = 0;

      //for (i = 0; i < 16; i++)
      //  ca->way[entry].data[way][i] = 0;

    }
  }
  return;
}

void cache_enable(cache_enty *ca)
{
  // cache enable does not clear the cache
  if (yabsys.use_sh2_cache) {
    ca->enable = 1;
  }
  else {
    ca->enable = 0;
  }
}

void cache_disable(cache_enty *ca)
{
  ca->enable = 0;
}

typedef struct
{
  u8 and;
  u8 or ;
} LRU_TABLE;

const LRU_TABLE lru_upd[4] = {{0x07, 0x00}, {0x19, 0x20}, {0x2A, 0x14}, {0x34, 0x0B}};

static const s8 lru_replace[0x40] =
    {
        0x03, 0x02, -1, 0x02, 0x03, -1, 0x01, 0x01, -1, 0x02, -1, 0x02, -1, -1, 0x01, 0x01,
        0x03, -1, -1, -1, 0x03, -1, 0x01, 0x01, -1, -1, -1, -1, -1, -1, 0x01, 0x01,
        0x03, 0x02, -1, 0x02, 0x03, -1, -1, -1, -1, 0x02, -1, 0x02, -1, -1, -1, -1,
        0x03, -1, -1, -1, 0x03, -1, -1, -1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

static INLINE void update_lru(int way, u32 *lru)
{
  *lru = ((*lru) & lru_upd[way].and) | lru_upd[way].or ;
  return;
}

static INLINE int select_way_to_replace(cache_enty *ca, u32 lru, u32 isInstr)
{
  return lru_replace[lru & ca->ccr_replace_and] | ca->ccr_replace_or[isInstr];
}

#if HAVE_BUILTIN_BSWAP16
#define SWAP16(v) (__builtin_bswap16(v))
#else
static INLINE u16 SWAP16(u16 v)
{
  return ((v >> 8) & 0x00FF) | ((v << 8) & 0xFF00);
}
#endif

#if HAVE_BUILTIN_BSWAP32
#define SWAP32(v) (__builtin_bswap32(v))
#else
static INLINE u32 SWAP32(u32 v)
{
  return ((v >> 24) & 0x000000FF) | ((v >> 8) & 0x0000FF00) | ((v << 8) & 0x00FF0000) | ((v << 24) & 0xFF000000);
}
#endif

void cache_memory_write_b(cache_enty *ca, u32 addr, u8 val, u32 *cycle)
{
  //if( (addr&0x0fffffff)==0x060ffca8 ) { 
  //  LOG("[%s] %d Write %zu-byte write of 0x%08x to 0x%08x PC=%08X frame %d:%d", CurrentSH2->isslave ? "SH2-S" : "SH2-M", CurrentSH2->cycles, 1, val, addr , CurrentSH2->regs.PC,  yabsys.frame_count, yabsys.LineCount );
  //}
  switch (addr & AREA_MASK)
  {
  case CACHE_USE:
  {
    if (ca->enable == 0)
    {
      MappedMemoryWriteByteNocache(addr, val, cycle);
      return;
    }
    const u32 tagaddr = (addr & TAG_MASK) | 0x02;
    const u32 entry = (addr & ENTRY_MASK) >> ENTRY_SHIFT;

    int way = -1;
    if (ca->way[entry].tag[3] == tagaddr)
    {
      way = 3;
    }
    else if (ca->way[entry].tag[2] == tagaddr)
    {
      way = 2;
    }
    else if (ca->way[entry].tag[1] == tagaddr)
    {
      way = 1;
    }
    else if (ca->way[entry].tag[0] == tagaddr)
    {
      way = 0;
    }

#ifdef CACHE_STATICS
    ca->write_count++;
#endif
    if (way > -1)
    {
      ca->way[entry].data[way][(addr & LINE_MASK)] = val;
      update_lru(way, &ca->lru[entry]);
      CACHE_LOG("[%s] %d Cache Write 4 %08X %d:%d:%d %08X\n", CurrentSH2->isslave ? "SH2-S" : "SH2-M", CurrentSH2->cycles, addr, entry, way, (addr & LINE_MASK), val);
    }
    MappedMemoryWriteByteNocache(addr, val, NULL);
    break;
  } // THROUGH TO CACHE_THROUGH
  case CACHE_THROUGH:
    MappedMemoryWriteByteNocache(addr, val, NULL);
    break;
  case CACHE_DATA_ARRAY:
    DataArrayWriteByte(addr, val);
    break;
  default:
    MappedMemoryWriteByteNocache(addr, val, NULL);
    break;
  }
}

void cache_memory_write_w(cache_enty *ca, u32 addr, u16 val, u32 *cycle)
{
  if (0x060C8004 == (addr & 0x0FFFFFFF)) {
    LOG("[%s] %d Cache Write 2 PC=%08X addr=%08X val=%08X", CurrentSH2->isslave ? "SH2-S" : "SH2-M", CurrentSH2->cycles, CurrentSH2->regs.PC, addr, val);
  }


  //if( (addr&0x0fffffff)==0x060ffca8 ) { 
  //  LOG("[%s] Write %zu-byte write of 0x%08x to 0x%08x PC=%08X", CurrentSH2->isslave ? "SH2-S" : "SH2-M", 2, val, addr , CurrentSH2->regs.PC);
  //}
  switch (addr & AREA_MASK)
  {
  case CACHE_USE:
  {
    if (ca->enable == 0)
    {
      MappedMemoryWriteWordNocache(addr, val, cycle);
      return;
    }

    if ((addr & 0x01))
    {
      CACHE_LOG("[%s] data alignment error for 16bit %08X\n", CurrentSH2->isslave ? "SH2-S" : "SH2-M", addr);
      addr &= ~(0x01);
    }

    const u32 tagaddr = (addr & TAG_MASK) | 0x02;
    const u32 entry = (addr & ENTRY_MASK) >> ENTRY_SHIFT;

    int way = -1;
    if (ca->way[entry].tag[3] == tagaddr)
    {
      way = 3;
    }
    else if (ca->way[entry].tag[2] == tagaddr)
    {
      way = 2;
    }
    else if (ca->way[entry].tag[1] == tagaddr)
    {
      way = 1;
    }
    else if (ca->way[entry].tag[0] == tagaddr)
    {
      way = 0;
    }

    if (way > -1)
    {
      *(u16 *)(&ca->way[entry].data[way][(addr & LINE_MASK)]) = SWAP16(val);
      update_lru(way, &ca->lru[entry]);
      CACHE_LOG("[%s] %d Cache Write 4 %08X %d:%d:%d %08X\n", CurrentSH2->isslave ? "SH2-S" : "SH2-M", CurrentSH2->cycles, addr, entry, way, (addr & LINE_MASK), val);
    }

#ifdef CACHE_STATICS
    ca->write_count++;
#endif
    MappedMemoryWriteWordNocache(addr, val, NULL);
    break;
  } // THROUGH TO CACHE_THROUGH
  case CACHE_THROUGH:
  {
    MappedMemoryWriteWordNocache(addr, val, NULL);
  }
  break;
  case CACHE_DATA_ARRAY:
    DataArrayWriteWord(addr, val);
    break;
  default:
    MappedMemoryWriteWordNocache(addr, val, NULL);
    break;
  }
}

void cache_memory_write_l(cache_enty *ca, u32 addr, u32 val, u32 *cycle)
{

  if ( 0x060C8004 == (addr & 0x0FFFFFFF)) {
    LOG("[%s] %d Cache Write 4 PC=%08X addr=%08X val=%08X", CurrentSH2->isslave ? "SH2-S" : "SH2-M", CurrentSH2->cycles, CurrentSH2->regs.PC,  addr, val);
  }

  switch (addr & AREA_MASK)
  {
  case CACHE_PURGE: // associative purge
  {
    int i;
    const u32 tagaddr = (addr & TAG_MASK) | 0x02;
    const u32 entry = (addr & ENTRY_MASK) >> ENTRY_SHIFT;

    CACHE_LOG("Cache purge %08X %d\n", addr, entry);

    for (i = 0; i < 4; i++)
    {
      if (ca->way[entry].tag[i] == tagaddr)
      {
        // only v bit is changed, the rest of the data remains
        ca->way[entry].tag[i] &= ~0x02;
      }
    }
  }
  break;
  case CACHE_USE:
  {
    if (ca->enable == 0)
    {
      MappedMemoryWriteLongNocache(addr, val, cycle);
      return;
    }

    if ((addr & 0x03))
    {
      addr &= ~(0x03);
      CACHE_LOG("[%s] data alignment error for 32bit %08X\n", CurrentSH2->isslave ? "SH2-S" : "SH2-M", addr);
    }

    const u32 tagaddr = (addr & TAG_MASK) | 0x02;
    const u32 entry = (addr & ENTRY_MASK) >> ENTRY_SHIFT;
    int way = -1;

    if (ca->way[entry].tag[3] == tagaddr)
    {
      way = 3;
    }
    else if (ca->way[entry].tag[2] == tagaddr)
    {
      way = 2;
    }
    else if (ca->way[entry].tag[1] == tagaddr)
    {
      way = 1;
    }
    else if (ca->way[entry].tag[0] == tagaddr)
    {
      way = 0;
    }

    if (way > -1)
    {
      *(u32 *)(&ca->way[entry].data[way][(addr & LINE_MASK)]) = SWAP32(val);
      update_lru(way, &ca->lru[entry]);
      CACHE_LOG("[%s] %d Cache Write 4 %08X %d:%d:%d %08X\n", CurrentSH2->isslave ? "SH2-S" : "SH2-M", CurrentSH2->cycles, addr, entry, way, (addr & LINE_MASK), val);
    }

#ifdef CACHE_STATICS
    ca->write_count++;
#endif
    MappedMemoryWriteLongNocache(addr, val, NULL);
    break;
  } // THROUGH TO CACHE_THROUGH
  case CACHE_THROUGH:
    MappedMemoryWriteLongNocache(addr, val, NULL);
    break;
  case CACHE_ADDRES_ARRAY:
    if (cycle != NULL) { *cycle = 14; }
    AddressArrayWriteLong(addr, val);
    break;
  case CACHE_DATA_ARRAY:
    DataArrayWriteLong(addr, val);
    break;
  default:
    MappedMemoryWriteLongNocache(addr, val, NULL);
    break;
  }
}

u8 cache_memory_read_b(cache_enty *ca, u32 addr, u32 *cycle)
{
  switch (addr & AREA_MASK)
  {
  case CACHE_USE:
  {
    int i = 0;
    int lruway = 0;
    if (ca->enable == 0)
    {
      return MappedMemoryReadByteNocache(addr, cycle);
    }
    const u32 tagaddr = (addr & TAG_MASK) | 0x02;
    const u32 entry = (addr & ENTRY_MASK) >> ENTRY_SHIFT;

    int way = -1;
    if (ca->way[entry].tag[3] == tagaddr)
    {
      way = 3;
    }
    else if (ca->way[entry].tag[2] == tagaddr)
    {
      way = 2;
    }
    else if (ca->way[entry].tag[1] == tagaddr)
    {
      way = 1;
    }
    else if (ca->way[entry].tag[0] == tagaddr)
    {
      way = 0;
    }

    if (way > -1)
    {
#ifdef CACHE_STATICS
      ca->read_hit_count++;
#endif
      update_lru(way, &ca->lru[entry]);
      const u8 rtn = ca->way[entry].data[way][(addr & LINE_MASK)];
#ifdef COHERENCY_CHECK
      u8 real = MappedMemoryReadByteNocache(addr, NULL);
      if (real != rtn) {
        LOG("[SH2-%s] %d Cache coherency ERROR 1 %08X %d:%d:%d cache = %02X real = %02X\n", CurrentSH2->isslave ? "S" : "M", CurrentSH2->cycles, addr, entry, way, (addr & LINE_MASK), rtn,real);
      }
#endif
      return rtn;
    }
#ifdef CACHE_STATICS
    ca->read_miss_count++;
#endif
    lruway = select_way_to_replace(ca,ca->lru[entry], 0);
    if(lruway >= 0)
    {
      update_lru(lruway, &ca->lru[entry]);
      ca->way[entry].tag[lruway] = tagaddr;
      u32 tmpcycle = 0;
      for (i = 0; i < 16; i += 4)
      {
        u32 odi = (addr + 4 + i) & 0xC;
        u32 ccycle = 0;
        ca->way[entry].data[lruway][odi] = MappedMemoryReadByteNocache( (addr & 0xFFFFFFF0) + odi, &ccycle);
        tmpcycle = ccycle << 1;
        ca->way[entry].data[lruway][odi + 1] = ReadByteList[(addr >> 16) & 0xFFF]((addr & 0xFFFFFFF0) + odi + 1);
        ca->way[entry].data[lruway][odi + 2] = ReadByteList[(addr >> 16) & 0xFFF]((addr & 0xFFFFFFF0) + odi + 2);
        ca->way[entry].data[lruway][odi + 3] = ReadByteList[(addr >> 16) & 0xFFF]((addr & 0xFFFFFFF0) + odi + 3);
        //CACHE_LOG("[SH2-%s] %d Cache miss read %08X %d:%d:%d", CurrentSH2->isslave ? "S" : "M", CurrentSH2->cycles, addr, entry, lruway, odi);
      }
      if (cycle) { *cycle = MIN(MAX_CACHE_MISS_CYCLE, tmpcycle);}

      CACHE_LOG("[SH2-%s] %d+%d Cache miss read 1 %08X\n", CurrentSH2->isslave ? "S" : "M", CurrentSH2->cycles, tmpcycle, addr);

      return ca->way[entry].data[lruway][addr & LINE_MASK];
    }
    else
    {
      return MappedMemoryReadByteNocache(addr, cycle);
    }
  }
  break;
  case CACHE_THROUGH:
    {
      const u8 rtn = MappedMemoryReadByteNocache(addr, cycle);
      //if (cycle != NULL /*&& (  (addr&0x0FFFFFFF) ==0x060FFC44 || addr ==0x260E3CB8 )*/ ) { 
      //  LOG("[SH2-%s] %d+%d Read 1-byte addr:%08X val:%08X PC=%08X frame=%d:%d" , CurrentSH2->isslave ? "S" : "M", CurrentSH2->cycles, *cycle, addr, rtn, CurrentSH2->regs.PC, yabsys.frame_count, yabsys.LineCount); 
      //}
      return rtn;
    }
    break;
  default:
    return MappedMemoryReadByteNocache(addr, cycle);
    break;
  }
  return 0;
}

u16 cache_memory_read_w(cache_enty *ca, u32 addr, u32 *cycle, u32 isInst)
{

  switch (addr & AREA_MASK)
  {
  case CACHE_USE:
  {
    int i = 0;
    int lruway = 0;
    if (ca->enable == 0)
    {
      return MappedMemoryReadWordNocache(addr, cycle);
    }

    if ((addr & 0x01))
    {
      CACHE_LOG("[%s] data alignment error for 16bit %08X\n", CurrentSH2->isslave ? "SH2-S" : "SH2-M", addr);
      addr &= ~(0x01);
    }

    const u32 tagaddr = (addr & TAG_MASK) | 0x02;
    const u32 entry = (addr & ENTRY_MASK) >> ENTRY_SHIFT;

    int way = -1;
    if (ca->way[entry].tag[3] == tagaddr)
    {
      way = 3;
    }
    else if (ca->way[entry].tag[2] == tagaddr)
    {
      way = 2;
    }
    else if (ca->way[entry].tag[1] == tagaddr)
    {
      way = 1;
    }
    else if (ca->way[entry].tag[0] == tagaddr)
    {
      way = 0;
    }

    if (way > -1)
    {
#ifdef CACHE_STATICS
      ca->read_hit_count++;
#endif
      update_lru(way, &ca->lru[entry]);
      u16 rtn = SWAP16(*(u16 *)(&ca->way[entry].data[way][(addr & LINE_MASK)]));
#ifdef COHERENCY_CHECK
      u16 real = MappedMemoryReadWordNocache(addr, NULL);
      if (real != rtn) {
        LOG("[SH2-%s] %d Cache coherency ERROR 2 %08X %d:%d:%d cache = %04X real = %04X\n", CurrentSH2->isslave ? "S" : "M", CurrentSH2->cycles, addr, entry, way, (addr & LINE_MASK), rtn, real);
      }
#endif

      return rtn;
    }

#ifdef CACHE_STATICS
    ca->read_miss_count++;
#endif

    lruway = select_way_to_replace(ca,ca->lru[entry], isInst);
    if (lruway >= 0)
    {
      update_lru(lruway, &ca->lru[entry]);
      ca->way[entry].tag[lruway] = tagaddr;

      u32 tmpcycle = 0;
      for (i = 0; i < 16; i += 4)
      {
        u32 odi = (addr + 4 + i) & 0xC;
        u32 ccycle = 0;
        *(u16 *)(&ca->way[entry].data[lruway][odi]) = SWAP16(MappedMemoryReadWordNocache((addr & 0xFFFFFFF0) + odi, &ccycle));
        tmpcycle = ccycle << 1;
        *(u16 *)(&ca->way[entry].data[lruway][odi + 2]) = SWAP16(ReadWordList[(addr >> 16) & 0xFFF]((addr & 0xFFFFFFF0) + odi + 2));
        //CACHE_LOG("[SH2-%s] %d Cache miss read %08X %d:%d:%d", CurrentSH2->isslave ? "S" : "M", CurrentSH2->cycles, addr, entry, lruway, odi);
      }
      if (cycle) { *cycle = MIN(MAX_CACHE_MISS_CYCLE, tmpcycle);}
      CACHE_LOG("[SH2-%s] %d+%d Cache miss read 2 %08X\n", CurrentSH2->isslave ? "S" : "M", CurrentSH2->cycles, tmpcycle, addr);
      u16 rtn = SWAP16(*(u16 *)(&ca->way[entry].data[lruway][(addr & LINE_MASK)]));
      return rtn;
    }
    else
    {
      return MappedMemoryReadWordNocache(addr, cycle);
    }
  }
  break;
  case CACHE_THROUGH:
    {
      const u16 rtn = MappedMemoryReadWordNocache(addr, cycle);
      //if (cycle != NULL /*&& (  (addr&0x0FFFFFFF) ==0x060FFC44 || addr ==0x260E3CB8 )*/ ) { 
      //  LOG("[SH2-%s] %d+%d Read 2-byte addr:%08X val:%08X PC=%08X frame=%d:%d" , CurrentSH2->isslave ? "S" : "M", CurrentSH2->cycles, *cycle, addr, rtn, CurrentSH2->regs.PC, yabsys.frame_count, yabsys.LineCount); 
      //}
      return rtn;
    }
    break;
  default:
    return MappedMemoryReadWordNocache(addr, cycle);
    break;
  }
  return 0;
}

u32 cache_memory_read_l(cache_enty *ca, u32 addr, u32 *cycle)
{

  switch (addr & AREA_MASK)
  {
  case CACHE_USE:
  {
    int i = 0;
    int lruway = 0;
    if (ca->enable == 0 )
    {
      return MappedMemoryReadLongNocache(addr, cycle);
    }

    if ((addr & 0x03))
    {
      CACHE_LOG("[%s] data alignment error for 32bit %08X\n", CurrentSH2->isslave ? "SH2-S" : "SH2-M", addr);
      addr &= ~(0x03);
    }

    const u32 tagaddr = (addr & TAG_MASK) | 0x02;
    const u32 entry = (addr & ENTRY_MASK) >> ENTRY_SHIFT;

    int way = -1;
    if (ca->way[entry].tag[3] == tagaddr)
    {
      way = 3;
    }
    else if (ca->way[entry].tag[2] == tagaddr)
    {
      way = 2;
    }
    else if (ca->way[entry].tag[1] == tagaddr)
    {
      way = 1;
    }
    else if (ca->way[entry].tag[0] == tagaddr)
    {
      way = 0;
    }

    if (way > -1)
    {
#ifdef CACHE_STATICS
      ca->read_hit_count++;
#endif
      update_lru(way, &ca->lru[entry]);
      u32 rtn = SWAP32(*(u32 *)(&ca->way[entry].data[way][(addr & LINE_MASK)]));

#ifdef COHERENCY_CHECK
      u32 real = MappedMemoryReadLongNocache(addr, NULL);
      if (real != rtn) {
        LOG("[SH2-%s] %d Cache coherency ERROR 4 %08X %d:%d:%d cache = %08X real = %08X", CurrentSH2->isslave ? "S" : "M", CurrentSH2->cycles, addr, entry, way, (addr & LINE_MASK), rtn, real);
    }
#endif

      //if ( (addr&0x0FFFFFFF) ==0x06043214) {
      //  LOG("[SH2-%s] %d+%d Read 4-byte %08x from %08x  PC=%08X frame=%d:%d" , CurrentSH2->isslave ? "S" : "M", CurrentSH2->cycles, *cycle, rtn, addr,  CurrentSH2->regs.PC, yabsys.frame_count, yabsys.LineCount);
      //}

      return rtn;
    }
#ifdef CACHE_STATICS
    ca->read_miss_count++;
#endif
    // cache miss
    lruway = select_way_to_replace(ca,ca->lru[entry], 0);
    if (lruway >= 0)
    {

      update_lru(lruway, &ca->lru[entry]);
      ca->way[entry].tag[lruway] = tagaddr;
      u32 tmpcycle = 0;
      for (i = 0; i < 16; i += 4)
      {
        u32 odi = (addr + 4 + i) & 0xC;
        u32 ccycle = 0;
        u32 data = MappedMemoryReadLongNocache((addr & 0xFFFFFFF0) + odi, &ccycle);
        *(u32 *)(&ca->way[entry].data[lruway][odi]) = SWAP32(data);
        tmpcycle = ccycle << 1;
        //CACHE_LOG("[SH2-%s] %d Cache miss read %08X %d:%d:%d %08X\n", CurrentSH2->isslave ? "S" : "M", CurrentSH2->cycles, addr, entry, lruway, odi, data);
      }
      if (cycle) { *cycle = MIN(MAX_CACHE_MISS_CYCLE, tmpcycle);}
      CACHE_LOG("[SH2-%s] %d+%d Cache miss read 4 %08X\n", CurrentSH2->isslave ? "S" : "M", CurrentSH2->cycles, tmpcycle, addr);
      return SWAP32(*(u32 *)(&ca->way[entry].data[lruway][(addr & LINE_MASK)]));
    }
    else
    {
      u32 rtn = MappedMemoryReadLongNocache(addr, cycle);
      return rtn;
    }
  }
  break;
  case CACHE_THROUGH:
    {
      const u32 rtn = MappedMemoryReadLongNocache(addr, cycle);
      //PC=%08X frame=%d:%d", CurrentSH2->isslave ? "SH2-S" : "SH2-M",  CurrentSH2->cycles, 4, val, addr , CurrentSH2->regs.PC, yabsys.frame_count, yabsys.LineCount );
      //if (cycle != NULL /*&& (  (addr&0x0FFFFFFF) ==0x060FFC44 || addr ==0x260E3CB8 )*/ ) { 
      //  LOG("[SH2-%s] %d+%d Read 4-byte addr:%08X val:%08X PC=%08X frame=%d:%d" , CurrentSH2->isslave ? "S" : "M", CurrentSH2->cycles, *cycle, addr, rtn, CurrentSH2->regs.PC, yabsys.frame_count, yabsys.LineCount); 
      //}
      return rtn;
    }
    break;
  default:
    return MappedMemoryReadLongNocache(addr, cycle);
    break;
  }
  return 0;
}

//////////////////////////////////////////////////////////////////////////////

u32 FASTCALL AddressArrayReadLong(u32 addr)
{
#ifdef CACHE_ENABLE
  int way = (CurrentSH2->onchip.CCR >> 6) & 3;
  int entry = (addr & 0x3FC) >> 4;
  u32 data = CurrentSH2->onchip.cache.way[entry].tag[way];
  data |= CurrentSH2->onchip.cache.lru[entry] << 4;
  CACHE_LOG(cache_f, "[SH2-%s] Address Read %08X %d:%d:%d %08X\n", CurrentSH2->isslave ? "S" : "M", addr, entry, way, addr & 0x0F, data);
  return data;
#else
  return CurrentSH2->AddressArray[(addr & 0x3FC) >> 2];
#endif
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL AddressArrayWriteLong(u32 addr, u32 val)
{
#ifdef CACHE_ENABLE
  int way = (CurrentSH2->onchip.CCR >> 6) & 3;
  int entry = (addr & 0x3FC) >> 4;
  CurrentSH2->onchip.cache.way[entry].tag[way] = addr & 0x1FFFFC02;
  CurrentSH2->onchip.cache.lru[entry] = (val >> 4) & 0x3f;
  CACHE_LOG(cache_f, "[SH2-%s] Address Write %08X %d:%d:%d %08X\n", CurrentSH2->isslave ? "S" : "M", addr, entry, way, addr & 0x0F, val);
#else
  CurrentSH2->AddressArray[(addr & 0x3FC) >> 2] = val;
#endif
}

//////////////////////////////////////////////////////////////////////////////

u8 FASTCALL DataArrayReadByte(u32 addr)
{
#ifdef CACHE_ENABLE
  if (CurrentSH2->onchip.cache.enable) {
    int way = (addr >> 10) & 3;
    int entry = (addr >> 4) & 0x3f;
    u8 data = CurrentSH2->onchip.cache.way[entry].data[way][addr & 0xf];
    CACHE_LOG(cache_f, "[SH2-%s] DataArrayReadByte %08X %d:%d:%d %08X\n", CurrentSH2->isslave ? "S" : "M", addr, entry, way, addr & 0x0F, data);
    return data;
  }
  else {
    return T2ReadByte(CurrentSH2->DataArray, addr & 0xFFF);
  }
#else
  return T2ReadByte(CurrentSH2->DataArray, addr & 0xFFF);
#endif
}

//////////////////////////////////////////////////////////////////////////////

u16 FASTCALL DataArrayReadWord(u32 addr)
{
#ifdef CACHE_ENABLE
  int way = (addr >> 10) & 3;
  int entry = (addr >> 4) & 0x3f;
  u16 data = ((u16)(CurrentSH2->onchip.cache.way[entry].data[way][addr & 0xf]) << 8) | CurrentSH2->onchip.cache.way[entry].data[way][(addr & 0xf) + 1];
  CACHE_LOG(cache_f, "[SH2-%s] DataArrayReadWord %08X %d:%d:%d %08X\n", CurrentSH2->isslave ? "S" : "M", addr, entry, way, addr & 0x0F, data);
  return data;
#else
  return T2ReadWord(CurrentSH2->DataArray, addr & 0xFFF);
#endif
}

//////////////////////////////////////////////////////////////////////////////

u32 FASTCALL DataArrayReadLong(u32 addr)
{
#ifdef CACHE_ENABLE
  int way = (addr >> 10) & 3;
  int entry = (addr >> 4) & 0x3f;
  u32 data = ((u32)(CurrentSH2->onchip.cache.way[entry].data[way][addr & 0xf]) << 24) |
      ((u32)(CurrentSH2->onchip.cache.way[entry].data[way][(addr & 0xf) + 1]) << 16) |
      ((u32)(CurrentSH2->onchip.cache.way[entry].data[way][(addr & 0xf) + 2]) << 8) |
      ((u32)(CurrentSH2->onchip.cache.way[entry].data[way][(addr & 0xf) + 3]) << 0);
    //LOG("[SH2-%s] DataArrayReadLong %08X %d:%d:%d %08X\n", CurrentSH2->isslave ? "S" : "M", addr, entry, way, addr & 0x0F, data);
  return data;
#else
  return T2ReadLong(CurrentSH2->DataArray, addr & 0xFFF);
#endif
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL DataArrayWriteByte(u32 addr, u8 val)
{
#ifdef CACHE_ENABLE
  int way = (addr >> 10) & 3;
  int entry = (addr >> 4) & 0x3f;
  CurrentSH2->onchip.cache.way[entry].data[way][addr & 0xf] = val;
#else
  T2WriteByte(CurrentSH2->DataArray, addr & 0xFFF, val);
#endif
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL DataArrayWriteWord(u32 addr, u16 val)
{
#ifdef CACHE_ENABLE
  int way = (addr >> 10) & 3;
  int entry = (addr >> 4) & 0x3f;
  CurrentSH2->onchip.cache.way[entry].data[way][addr & 0xf] = val >> 8;
  CurrentSH2->onchip.cache.way[entry].data[way][(addr & 0xf) + 1] = val;
#else
  T2WriteWord(CurrentSH2->DataArray, addr & 0xFFF, val);
#endif
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL DataArrayWriteLong(u32 addr, u32 val)
{
#ifdef CACHE_ENABLE
    int way = (addr >> 10) & 3;
    int entry = (addr >> 4) & 0x3f;
    CurrentSH2->onchip.cache.way[entry].data[way][(addr & 0xf)] = ((val >> 24) & 0xFF);
    CurrentSH2->onchip.cache.way[entry].data[way][(addr & 0xf) + 1] = ((val >> 16) & 0xFF);
    CurrentSH2->onchip.cache.way[entry].data[way][(addr & 0xf) + 2] = ((val >> 8) & 0xFF);
    CurrentSH2->onchip.cache.way[entry].data[way][(addr & 0xf) + 3] = ((val >> 0) & 0xFF);
#else
  T2WriteLong(CurrentSH2->DataArray, addr & 0xFFF, val);
#endif
}
