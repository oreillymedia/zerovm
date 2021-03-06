/*
 * Copyright (c) 2012, LiteStack, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <assert.h>
#include <sys/mman.h>
#include "src/channels/channel.h"
#include "src/main/report.h"
#include "src/platform/sel_memory.h"
#include "src/main/setup.h"
#include "src/syscalls/daemon.h"

static int idx[] = {TrapRead, TrapWrite, TrapJail, TrapUnjail, TrapExit, TrapFork};
static char *function[] =
  {"TrapRead", "TrapWrite", "TrapJail", "TrapUnjail", "TrapExit", "TrapFork", "n/a"};

/*
 * check "prot" access for user area (start, size)
 * if failed return -1, otherwise - 0
 */
static int CheckRAMAccess(struct NaClApp *nap, uintptr_t start, int64_t size, int prot)
{
  int i;

  start = NaClUserToSysAddrNullOkay(nap, start);
  for(i = LeftBumperIdx; i < MemMapSize; ++i)
  {
    /* skip until start hit block in mem_map */
    if(start >= nap->mem_map[i].end) continue;

    /* fail if block protection doesn't meat prot */
    if((prot & nap->mem_map[i].prot) == 0) return -1;

    /* subtract checked space from given area */
    size -= (nap->mem_map[i].end - start);
    if(size <= 0) return 0;

    start = nap->mem_map[i].end;
  }
  return -1;
}

/*
 * read specified amount of bytes from given desc/offset to buffer
 * return amount of read bytes or negative error code if call failed
 */
static int32_t ZVMReadHandle(struct NaClApp *nap,
    int ch, char *buffer, int32_t size, int64_t offset)
{
  struct ChannelDesc *channel;
  int64_t tail;
  char *sys_buffer;

  assert(nap != NULL);
  assert(nap->manifest != NULL);
  assert(nap->manifest->channels != NULL);

  /* check the channel number */
  if(ch < 0 || ch >= nap->manifest->channels->len)
  {
    ZLOGS(LOG_DEBUG, "channel_id=%d, buffer=%p, size=%d, offset=%ld",
        ch, buffer, size, offset);
    return -EINVAL;
  }
  channel = CH_CH(nap->manifest, ch);
  ZLOGS(LOG_INSANE, "channel %s, buffer=%p, size=%d, offset=%ld",
      channel->alias, buffer, size, offset);

  /* check buffer and convert address */
  if(CheckRAMAccess(nap, (uintptr_t)buffer, size, PROT_WRITE) == -1) return -EINVAL;
  sys_buffer = (char*)NaClUserToSys(nap, (uintptr_t)buffer);

  /* ignore user offset for sequential access read */
  if(CH_SEQ_READABLE(channel))
    offset = channel->getpos;
  else
  /* prevent reading beyond the end of the random access channels */
    size = MIN(channel->size - offset, size);

  /* check arguments sanity */
  if(size == 0) return 0; /* success. user has read 0 bytes */
  if(size < 0) return -EFAULT;
  if(offset < 0) return -EINVAL;

  /* check for eof */
  if(channel->eof) return 0;

  /* check limits */
  if(channel->counters[GetsLimit] >= channel->limits[GetsLimit])
    return -EDQUOT;
  if(CH_RND_READABLE(channel))
    if(offset >= channel->limits[PutSizeLimit] - channel->counters[PutSizeLimit]
      + channel->size) return -EINVAL;

  /* calculate i/o leftovers */
  tail = channel->limits[GetSizeLimit] - channel->counters[GetSizeLimit];
  if(size > tail) size = tail;
  if(size < 1) return -EDQUOT;

  /* read data */
  return ChannelRead(channel, sys_buffer, (size_t)size, (off_t)offset);
}

/*
 * write specified amount of bytes from buffer to given desc/offset
 * return amount of read bytes or negative error code if call failed
 */
static int32_t ZVMWriteHandle(struct NaClApp *nap,
    int ch, const char *buffer, int32_t size, int64_t offset)
{
  struct ChannelDesc *channel;
  int64_t tail;
  const char *sys_buffer;

  assert(nap != NULL);
  assert(nap->manifest != NULL);
  assert(nap->manifest->channels != NULL);

  /* check the channel number */
  if(ch < 0 || ch >= nap->manifest->channels->len)
  {
    ZLOGS(LOG_DEBUG, "channel_id=%d, buffer=%p, size=%d, offset=%ld",
        ch, buffer, size, offset);
    return -EINVAL;
  }
  channel = CH_CH(nap->manifest, ch);
  ZLOGS(LOG_INSANE, "channel %s, buffer=%p, size=%d, offset=%ld",
      channel->alias, buffer, size, offset);

  /* check buffer and convert address */
  if(CheckRAMAccess(nap, (uintptr_t)buffer, size, PROT_READ) == -1) return -EINVAL;
  sys_buffer = (char*)NaClUserToSys(nap, (uintptr_t) buffer);

  /* ignore user offset for sequential access write */
  if(CH_SEQ_WRITEABLE(channel)) offset = channel->putpos;

  /* check arguments sanity */
  if(size == 0) return 0; /* success. user has read 0 bytes */
  if(size < 0) return -EFAULT;
  if(offset < 0) return -EINVAL;

  /* check limits */
  if(channel->counters[PutsLimit] >= channel->limits[PutsLimit])
    return -EDQUOT;
  tail = channel->limits[PutSizeLimit] - channel->counters[PutSizeLimit];
  if(offset >= channel->limits[PutSizeLimit] &&
      !((CH_RW_TYPE(channel) & 1) == 1)) return -EINVAL;

  if(offset >= channel->size + tail) return -EINVAL;
  if(size > tail) size = tail;
  if(size < 1) return -EDQUOT;

  /* write data */
  return ChannelWrite(channel, sys_buffer, (size_t)size, (off_t)offset);
}

#define JAIL_CHECK \
    uintptr_t sysaddr; \
    int result; \
\
    assert(nap != NULL); \
    assert(nap->manifest != NULL); \
\
    sysaddr = NaClUserToSysAddrNullOkay(nap, addr); \
\
    /* sanity check */ \
    if(size <= 0) return -EINVAL; \
    if(sysaddr < nap->mem_map[HeapIdx].start || \
        sysaddr >= nap->mem_map[HeapIdx].end) return -EINVAL; \
    if(sysaddr != ROUNDDOWN_64K(sysaddr)) return -EINVAL

/*
 * validate given buffer and, if successful, change protection to
 * read / execute and return 0
 */
static int32_t ZVMJailHandle(struct NaClApp *nap, uintptr_t addr, int32_t size)
{
  JAIL_CHECK;

  /* validate */
  result = NaClSegmentValidates((uint8_t*)sysaddr, size, sysaddr);
  if(result == 0) return -EPERM;

  /* protect */
  result = NaCl_mprotect((void*)sysaddr, size, PROT_READ | PROT_EXEC);
  if(result != 0) return -EACCES;

  return 0;
}

/* change protection to read / write and return 0 if successful */
static int32_t ZVMUnjailHandle(struct NaClApp *nap, uintptr_t addr, int32_t size)
{
  JAIL_CHECK;

  /* protect */
  result = NaCl_mprotect((void*)sysaddr, size, PROT_READ | PROT_WRITE);
  if(result != 0) return -EACCES;

  return 0;
}
#undef JAIL_CHECK

/* return index of function id in "function" */
static int FunctionIndexById(int id)
{
  int i;

  for(i = 0; i < ARRAY_SIZE(idx); ++i)
    if(idx[i] == id) return i;
  return ARRAY_SIZE(idx);
}

static void SyscallZTrace(int i, ...)
{
  char *msg;
  va_list ap;
  char *fmt[] = {"%s(%d, %p, %d, %ld)", "%s(%d, %p, %d, %ld)",
      "%s(%p, %d)", "%s(%p, %d)", "%s(%d)", "%s()"};

  va_start(ap, i);
  msg = g_strdup_vprintf(fmt[i], ap);
  va_end(ap);
  ZTrace(msg);
  g_free(msg);
}

/* user exit. session is finished */
static void ZVMExitHandle(struct NaClApp *nap, int32_t code)
{
  assert(nap != NULL);

  SetUserCode(code);
  if(GetExitCode() == 0)
    SetExitState(OK_STATE);
  ZLOGS(LOG_DEBUG, "SESSION %d RETURNED %d", nap->manifest->node, code);
  SyscallZTrace(4, function[4], code);
  ReportDtor(0);
}

int32_t TrapHandler(struct NaClApp *nap, uint32_t args)
{
  uint64_t *sargs;
  int retcode = 0;
  int i;

  assert(nap != NULL);
  assert(nap->manifest != NULL);

  /*
   * translate address from user space to system
   * note: cannot set "trap error"
   */
  sargs = (uint64_t*)NaClUserToSys(nap, (uintptr_t)args);
  i = FunctionIndexById(*sargs);
  ZLOGS(LOG_DEBUG, "%s called", function[i]);
  ZTrace("untrusted code");

  switch(*sargs)
  {
    case TrapFork:
      if(Daemon(nap) == 0)
      {
        SyscallZTrace(5, function[5]);
        ZVMExitHandle(nap, 0);
      }
      break;
    case TrapExit:
      ZVMExitHandle(nap, (int32_t)sargs[2]);
      break;
    case TrapRead:
      retcode = ZVMReadHandle(nap,
          (int)sargs[2], (char*)sargs[3], (int32_t)sargs[4], sargs[5]);
      break;
    case TrapWrite:
      retcode = ZVMWriteHandle(nap,
          (int)sargs[2], (char*)sargs[3], (int32_t)sargs[4], sargs[5]);
      break;
    case TrapJail:
      retcode = ZVMJailHandle(nap, (uint32_t)sargs[2], (int32_t)sargs[3]);
      break;
    case TrapUnjail:
      retcode = ZVMUnjailHandle(nap, (uint32_t)sargs[2], (int32_t)sargs[3]);
      break;
    default:
      retcode = -EPERM;
      ZLOG(LOG_ERROR, "function %ld is not supported", *sargs);
      break;
  }

  /* report, ztrace and return */
  FastReport();
  ZLOGS(LOG_DEBUG, "%s returned %d", function[i], retcode);
  SyscallZTrace(i, function[i], sargs[2], sargs[3], sargs[4], sargs[5]);
  return retcode;
}
