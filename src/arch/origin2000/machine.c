/*****************************************************************************
 * $Source$
 * $Author$
 * $Date$
 * $Revision$
 *****************************************************************************/

#include <sys/types.h>
#include <sys/sysmp.h>
#include <sys/sysinfo.h>
#include <sys/prctl.h>
#include <sys/pda.h>
#include <ulocks.h>
#include <math.h>
#include <stdlib.h>
#include "converse.h"
#include <time.h>
#include "fifo.h"

usptr_t *arena;
static barrier_t *barr;

#define BLK_LEN  512
typedef struct {
  usema_t  *sema;
  void     **blk;
  unsigned int blk_len;
  unsigned int first;
  unsigned int len;
  unsigned int maxlen;
} McQueue;

static McQueue *McQueueCreate(void);
static void McQueueAddToBack(McQueue *queue, void *element);
static void *McQueueRemoveFromFront(McQueue *queue);
static McQueue **MsgQueue;
#define g_malloc(s)  usmalloc(s,arena)
#define g_free(p)    usfree(p,arena)

#if NODE_0_IS_CONVHOST
int inside_comm = 0;
#endif

CpvDeclare(void*, CmiLocalQueue);
int Cmi_mype;
int Cmi_numpes;
int Cmi_myrank;

static int nthreads;
static int requested_npe;
static int arena_size_meg;

static void threadInit(void *arg);

int membusy;

void CmiMemLock() {membusy=1;}
void CmiMemUnlock() {membusy=0;}

/***********************************************************************
 *
 * Abort function:
 *
 ************************************************************************/

void CmiAbort(const char *message)
{
  CmiError(message);
  exit(1);
}

int CmiAsyncMsgSent(CmiCommHandle msgid)
{
  return 1;
}


typedef struct {
  void       *argv;
  CmiStartFn fn;
  int        argc;
  int        npe;
  int        mype;
  int        usched;
  int        initret;
} USER_PARAMETERS;

USER_PARAMETERS usrparam;

static int CountArgs(char **argv)
{
  int count=0;
  while (argv[0]) { count++; argv++; }
  return count;
}

static void RemoveArgv(char** argv, int i, int *argc)
{
  int j;

  for(j=i+1; argv[j] != 0; j++)
    argv[j-1] = argv[j];
  argv[j-1] = 0;
  (*argc)--;
}

void ConverseInit(int argc, char **argv, CmiStartFn fn, int usched, int initret)
{
  int i;
 
  requested_npe = 1; 
  arena_size_meg = 16;

  /* Parse Converse-specific arguments, stripping them out before
   * passing to the user code.
   * I thought this was already done elsewhere, but its not working,
   * so I will do it again.
   */
  i =  0;
  while (argv[i] != 0) {
    if (strncmp(argv[i], "+p",2) == 0) {
      if (strlen(argv[i]) > 2) {
	sscanf(argv[i], "+p%d", &requested_npe);
	RemoveArgv(argv,i,&argc);
      } else {
	RemoveArgv(argv,i,&argc);
	if (argv[i]) {  
	  /* Since we removed argv[i], argv[i] is now the
	   * next argument 
	   */
	  sscanf(argv[i], "%d", &requested_npe);
	  RemoveArgv(argv,i,&argc);
	}
      }
    } else if (strncmp(argv[i], "+memsize",8) == 0) {
      if (strlen(argv[i]) > 8 ) {
	sscanf(argv[i], "+memsize%d", &arena_size_meg);
	RemoveArgv(argv,i,&argc);
      } else {
	RemoveArgv(argv,i,&argc);
	if (argv[i] != 0) {
	  /* Since we removed argv[i], argv[i] is now the
	   * next argument 
	   */
	  sscanf(argv[i], "%d", &arena_size_meg);
	  RemoveArgv(argv,i,&argc);
	}
      }
    } else i++;  /* If we didn't remove the argument(s), we need to
		  * increment
		  */
  }

  if (requested_npe <= 0)
  {
    CmiError("Error: requested number of processors is invalid %d\n",
              requested_npe);
    abort();
  }

  usconfig(CONF_INITUSERS, requested_npe+1);
  usconfig(CONF_ARENATYPE, US_SHAREDONLY);
  if(usconfig(CONF_INITSIZE,  (arena_size_meg * 1<<20))==(-1)) {
    CmiPrintf("Cannot set size of arena\n");
    abort();
  }
  arena = usinit("/dev/zero");
  if(arena == (usptr_t *) NULL) {
    CmiError("Cannot allocate arena\n");
    abort();
  }
  barr = new_barrier(arena);
  init_barrier(barr);
  nthreads = requested_npe;

  usrparam.argc = argc;
  usrparam.argv = argv;
  usrparam.npe = requested_npe;
  usrparam.fn = fn;
  usrparam.initret = initret;
  usrparam.usched = usched;

  MsgQueue=(McQueue **)g_malloc(requested_npe*sizeof(McQueue *));
  if (MsgQueue == (McQueue **)0) {
    CmiError("Cannot Allocate Memory...\n");
    abort();
  }
  for(i=0; i<requested_npe; i++) 
    MsgQueue[i] = McQueueCreate();

  for(i=1; i<requested_npe; i++) {
    usrparam.mype = i;
    sproc(threadInit, PR_SFDS, (void *)&usrparam);
  }
  usrparam.mype = 0;
  threadInit(&usrparam);
}

/* static void neighbour_init(int); */

void CmiTimerInit(void);

static void threadInit(void *arg)
{
  USER_PARAMETERS *usrparam;
  usrparam = (USER_PARAMETERS *) arg;


  CpvInitialize(void*, CmiLocalQueue);
  Cmi_mype  = usrparam->mype;
  Cmi_myrank = 0;
  Cmi_numpes =  usrparam->npe;
#ifdef DEBUG
  printf("thread %d/%d started \n", CmiMyPe(), CmiNumPes());
#endif
  prctl(PR_SETEXITSIG, SIGHUP,0);

  CthInit(usrparam->argv);
  CpvAccess(CmiLocalQueue) = (void *) FIFO_Create();
  CmiTimerInit();
  /*  neighbour_init(Cmi_mype); */
  usadd(arena);
  ConverseCommonInit(usrparam->argv);
  if (usrparam->initret==0 || usrparam->mype) {
    usrparam->fn(CountArgs(usrparam->argv), usrparam->argv);
    if (usrparam->usched==0 || usrparam->mype ) {
      CsdScheduler(-1);
      ConverseExit();
      if(usrparam->mype==0) {
        for(int i=1;i<usrparam->npe;i++)
          wait(0);
      }
    }
  }
}


void ConverseExit(void)
{
#if (CMK_DEBUG_MODE || CMK_WEB_MODE || NODE_0_IS_CONVHOST)
  if (CmiMyPe() == 0){
    CmiPrintf("End of program\n");
  }
#endif
  prctl(PR_SETEXITSIG,0,0);
  ConverseCommonExit();
  barrier(barr, nthreads);
}


void CmiDeclareArgs(void)
{
}


void CmiNotifyIdle()
{
}

void *CmiGetNonLocal()
{
  return McQueueRemoveFromFront(MsgQueue[CmiMyPe()]);
}


void CmiSyncSendFn(int destPE, int size, char *msg)
{
  char *buf;

  buf=(void *)g_malloc(size+8);
  if(buf==(void *)0) {
    CmiError("Cannot allocate memory of size %d!\n", size);
    abort();
  }
  ((int *)buf)[0]=size;
  buf += 8;

#ifdef DEBUG
  printf("Message of size %d allocated\n", CmiSize(buf));
#endif
  memcpy(buf,msg,size);
  McQueueAddToBack(MsgQueue[destPE],buf); 
  CQdCreate(CpvAccess(cQdState), 1);
}


CmiCommHandle CmiAsyncSendFn(int destPE, int size, char *msg)
{
  CmiSyncSendFn(destPE, size, msg); 
  return 0;
}


void CmiFreeSendFn(int destPE, int size, char *msg)
{
  if (Cmi_mype==destPE) {
    CQdCreate(CpvAccess(cQdState), 1);
    FIFO_EnQueue(CpvAccess(CmiLocalQueue),msg);
  } else {
    CmiSyncSendFn(destPE, size, msg);
    CmiFree(msg);
  }
}

void CmiSyncBroadcastFn(int size, char *msg)
{
  int i;
  for(i=0; i<CmiNumPes(); i++)
    if (CmiMyPe() != i) CmiSyncSendFn(i,size,msg);
}

CmiCommHandle CmiAsyncBroadcastFn(int size, char *msg)
{
  CmiSyncBroadcastFn(size, msg);
  return 0;
}

void CmiFreeBroadcastFn(int size, char *msg)
{
  CmiSyncBroadcastFn(size,msg);
  CmiFree(msg);
}

void CmiSyncBroadcastAllFn(int size, char *msg)
{
  int i;
  for(i=0; i<CmiNumPes(); i++) 
    CmiSyncSendFn(i,size,msg);
}


CmiCommHandle CmiAsyncBroadcastAllFn(int size, char *msg)
{
  CmiSyncBroadcastAllFn(size, msg);
  return 0; 
}


void CmiFreeBroadcastAllFn(int size, char *msg)
{
  int i;
  for(i=0; i<CmiNumPes(); i++) {
    if(CmiMyPe() != i) {
      CmiSyncSendFn(i,size,msg);
    }
  }
  FIFO_EnQueue(CpvAccess(CmiLocalQueue),msg);
  CQdCreate(CpvAccess(cQdState), 1);
}

/* ****************************************************************** */
/*    The following internal functions implements FIFO queues for     */
/*    messages. These queues are shared among threads                 */
/* ****************************************************************** */

static void ** AllocBlock(unsigned int len)
{
  void ** blk;

  blk=(void **)g_malloc(len*sizeof(void *));
  if(blk==(void **)0) {
    CmiError("Cannot Allocate Memory!\n");
    abort();
  }
  return blk;
}

static void 
SpillBlock(void **srcblk, void **destblk, unsigned int first, unsigned int len)
{
  memcpy(destblk, &srcblk[first], (len-first)*sizeof(void *));
  memcpy(&destblk[len-first],srcblk,first*sizeof(void *));
}

McQueue * McQueueCreate(void)
{
  McQueue *queue;

  queue = (McQueue *) g_malloc(sizeof(McQueue));
  if(queue==(McQueue *)0) {
    CmiError("Cannot Allocate Memory!\n");
    abort();
  }
  queue->sema = usnewsema(arena, 1);
  usinitsema(queue->sema, 1);
  queue->blk = AllocBlock(BLK_LEN);
  queue->blk_len = BLK_LEN;
  queue->first = 0;
  queue->len = 0;
  queue->maxlen = 0;
  return queue;
}

void McQueueAddToBack(McQueue *queue, void *element)
{
  inside_comm = 1;
#ifdef DEBUG
  printf("[%d] Waiting for lock\n",CmiMyPe());
#endif
  uspsema(queue->sema);
#ifdef DEBUG
  printf("[%d] Acquired lock\n",CmiMyPe());
#endif
  if(queue->len==queue->blk_len) {
    void **blk;

    queue->blk_len *= 3;
    blk = AllocBlock(queue->blk_len);
    SpillBlock(queue->blk, blk, queue->first, queue->len);
    g_free(queue->blk);
    queue->blk = blk;
    queue->first = 0;
  }
  queue->blk[(queue->first+queue->len++)%queue->blk_len] = element;
  if(queue->len>queue->maxlen)
    queue->maxlen = queue->len;
  usvsema(queue->sema);
#ifdef DEBUG
  printf("[%d] released lock\n",CmiMyPe());
#endif
  inside_comm = 0;
}


void * McQueueRemoveFromFront(McQueue *queue)
{
  void *element = (void *) 0;
  void *localmsg = (void *) 0;
  if(queue->len) {
    uspsema(queue->sema);
    element = queue->blk[queue->first++];
    queue->first = (queue->first+queue->blk_len)%queue->blk_len;
    queue->len--;
    usvsema(queue->sema);
  }
  if(element) {
#ifdef DEBUG
    printf("[%d] received message of size %d\n", CmiMyPe(), CmiSize(element));
#endif
    localmsg = CmiAlloc(CmiSize(element));
    memcpy(localmsg, element, CmiSize(element));
    g_free((char *)element-8);
  }
  return localmsg;
}

/* Timer Routines */


CpvStaticDeclare(double,inittime_wallclock);
CpvStaticDeclare(double,inittime_virtual);

void CmiTimerInit(void)
{
  struct timespec temp;
  CpvInitialize(double, inittime_wallclock);
  CpvInitialize(double, inittime_virtual);
  clock_gettime(CLOCK_SGI_CYCLE, &temp);
  CpvAccess(inittime_wallclock) = (double) temp.tv_sec +
				  1e-9 * temp.tv_nsec;
  CpvAccess(inittime_virtual) = CpvAccess(inittime_wallclock);
}

double CmiWallTimer(void)
{
  struct timespec temp;
  double currenttime;

  clock_gettime(CLOCK_SGI_CYCLE, &temp);
  currenttime = (double) temp.tv_sec +
                1e-9 * temp.tv_nsec;
  return (currenttime - CpvAccess(inittime_wallclock));
}

double CmiCpuTimer(void)
{
  struct timespec temp;
  double currenttime;

  clock_gettime(CLOCK_SGI_CYCLE, &temp);
  currenttime = (double) temp.tv_sec +
                1e-9 * temp.tv_nsec;
  return (currenttime - CpvAccess(inittime_virtual));
}

double CmiTimer(void)
{
  return CmiCpuTimer();
}

