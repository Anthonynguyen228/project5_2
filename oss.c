/*
CS_4760
Anthony Nguyen
04/19/2022
*/


#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/ipc.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>

#include "oss.h"


static char* prog;
static char* logfile = "output.txt"; 
static unsigned int requestCount = 0;
static unsigned int acceptCount = 0;
static unsigned int denyCount = 0;
static unsigned int linesCount = 0;

static int shmID = -1, semID = -1;
static struct oss *ossptr = NULL;

static int aliveFlag = 1;
static struct ossReport report; 
static struct timeval clockTick; //the tick of our OSS system clock

static int blocked[processSize]; //blocked queue
static int blockedLength = 0;    //queue is empty at start
static int termFlag = 0;

static void deallocateSHM(){
	if(ossptr != NULL){
		if(shmdt(ossptr) == -1){
			fprintf(stderr,"%s: failed to detach shared memory. ",prog);
      perror("Error");
		}
	}
	if(shmID != -1){
		if(shmctl(shmID, IPC_RMID, NULL) == -1){
			fprintf(stderr,"%s: failed to delete shared memory. ",prog);
      perror("Error");
		}
	}
	if(semID != -1){
		if(semctl(semID, 0, IPC_RMID) == -1){
			fprintf(stderr,"%s: failed to delete semaphore.",prog);
      perror("Error");
		}	
	}
}
static void listSystemResources(){
  int i, j;

  printf("Master System available resources at time %li.%06d\n", ossptr->time.tv_sec, ossptr->time.tv_usec);
  linesCount++;

  //print resource title
  printf("    ");
  for (i = 0; i < descriptorCount; i++){
    printf("R%2d ", i);
  }
  printf("\n");

  //print max
  printf("MAX ");
  for (i = 0; i < descriptorCount; i++){
    printf("%*d ", 3, ossptr->desc[i].max);
  }
  printf("\n");

  //print available now
  printf("NOW ");
  for (i = 0; i < descriptorCount; i++){
    printf("%3d ", ossptr->desc[i].val);
  }
  printf("\n");
  linesCount += 3;

  //print what process have
  for (i = 0; i < processSize; i++){
    struct process *proc = &ossptr->procs[i];
    if (proc->pid > 0){
      printf("P%2d ", proc->id);

      for (j = 0; j < descriptorCount; j++){
        printf("%3d ", proc->desc[j].val);
      }
      printf("\n");
      linesCount++;
    }
  }
}
static void initReport(){
	report.pStart.curr = 0;
	report.pStart.max = processMaximum;
	report.pRun.curr = 0;
  report.pRun.max = processSize;
	report.pDone.curr = 0;
  report.pDone.max = processMaximum;
	
  report.waitTime.tv_sec = 0;
  report.waitTime.tv_usec = 0;

	report.immediateAccept = 0;
	report.waitAccept = 0;
	report.deadlockRun = 0;
}
static void printReport(){
  //restore the output, if it was sent to /dev/null
  stdout = freopen(logfile, "a", stdout);

  //list system resources at the end of the program
  if(report.pDone.curr == 0){
    report.pDone.curr = 1;
  }
  printf("*****************************************************\n");
	printf("\t\tOSS REPORT\n");
  listSystemResources();
  printf("\n\tRequests (include released requests): %d\n", requestCount);
  printf("\tAccepted asking for resources requests: %d\n", acceptCount);
  printf("\tDenied requests(because of deadlocks): %d\n", denyCount);
  printf("\tImmediate Granted Requests: %d\n", report.immediateAccept);
  printf("\tGranted Requests after waiting: %d\n", report.waitAccept);
  printf("\tDeadlock running: %d time(s)\n",report.deadlockRun);
  printf("\tAverage Wait Time: %lu:%06d\n", report.waitTime.tv_sec/report.pDone.curr, report.waitTime.tv_usec/report.pDone.curr);
  printf("*****************************************************\n");

}
//Lock the shared region in signal safe way
static int ossSemWait(){
  struct sembuf sop = {.sem_num = 0, .sem_flg = 0, .sem_op = -1};

  sigset_t mask, oldmask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGCHLD);
  sigprocmask(SIG_BLOCK, &mask, &oldmask);

  if (semop(semID, &sop, 1) == -1){
    fprintf(stderr,"%s: ",prog);
		perror("semop");
    sigprocmask(SIG_SETMASK, &oldmask, NULL);
    aliveFlag = 0;
    if(termFlag == 1){
      deallocateSHM();
      exit(EXIT_FAILURE);
    }else{
      termFlag = 1;
    }
  
  }

  sigprocmask(SIG_SETMASK, &oldmask, NULL);

  return 0;
}

//Signal to unlock the shared buffer
static int ossSemPost(){
  struct sembuf sop = {.sem_num = 0, .sem_flg = 0, .sem_op = 1};

  sigset_t mask, oldmask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGCHLD);
  sigprocmask(SIG_BLOCK, &mask, &oldmask);

  if (semop(semID, &sop, 1) == -1){
    fprintf(stderr,"%s: ",prog);
		perror("semop");
    aliveFlag = 0;
    if(termFlag == 1){
      deallocateSHM();
      exit(EXIT_FAILURE);
    }else{
      termFlag = 1;
    }
  }

  sigprocmask(SIG_SETMASK, &oldmask, NULL);

  return 0;
}

//Add process to blocked queue
static int enqueueBlocked(const int processID){

  if (blockedLength < processSize){
    blocked[blockedLength++] = processID;
    return 0;
  }else{
    return -1;
  }
}

//Remove a process from blocked queue
static int dequeueBlocked(const int index){
  int i;

  //check range
  if (index >= blockedLength){
    return -1;
  }

  //get process at particular index
  const int processID = blocked[index];
  --blockedLength;

  for (i = index; i < blockedLength; ++i){
    blocked[i] = blocked[i + 1];
  }
  blocked[i] = -1; //clear last queue slot

  return processID;
}

static void descriptorsInit(struct descriptor sys[descriptorCount]){
  int i;
  int sharedDesc = descriptorCount / (100 / sharedDescriptors);

  //decide which descriptor will be shared
  while (sharedDesc > 0){
    //generate random
    const int descriptorID = rand() % descriptorCount;

    if (sys[descriptorID].shareable == 0){                                  
      //if its not shared yet
      sys[descriptorID].shareable = 1; //make it shared
      --sharedDesc;
    }
  }

  //fill descriptor value
  for (i = 0; i < descriptorCount; i++){

    if (sys[i].shareable == 1){
      sys[i].max = 1 + (rand() % descriptorValueMax);
    }else{
      sys[i].max = 1;
    }
    sys[i].val = sys[i].max;
  }
}


static int ossTimeUpdate(struct timeval *update){
  if(aliveFlag){
    struct timeval tv;

    if (ossSemWait() < 0){
      return -1;
    }

    timeradd(&ossptr->time, update, &tv);
    ossptr->time = tv;
  }
  return ossSemPost();
}

//get process index by its id
static int idToIndex(const int id){
  int i;
  for (i = 0; i < processSize; i++){
    if (ossptr->procs[i].id == id){
      return i;
    }
  }
  return -1;
}

//get process index by its pid
static int pidToIndex(const int pid){
  int i;
  for (i = 0; i < processSize; i++){
    if (ossptr->procs[i].pid == pid){
      return i;
    }
  }
  return -1;
}

//Wait and clear all exited processes
static void ossWaitchild(){
  pid_t pid;
  int status, idx;

  while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0){

    idx = pidToIndex(pid);
    if (idx == -1){
      continue;
    }

    printf("Master process P%d terminated at time %lu:%06d\n",
           ossptr->procs[idx].id, ossptr->time.tv_sec, ossptr->time.tv_usec);
    linesCount++;

    //clear the processDone process
    bzero(&ossptr->procs[idx], sizeof(struct process));

    --report.pRun.curr;
    if (++report.pDone.curr >= report.pStart.max){
      printf("Master all processes exited. Stopping OSS\n");
      linesCount++;
      aliveFlag = 0;
    }
  }
}	
static void ossExit(){
  int i;

  if (ossptr){
    //some process may be waiting on timer
    ossptr->time.tv_sec++;

    //deny all requests
    ossSemWait();
    for (i = 0; i < processSize; i++){
      if ((ossptr->procs[i].request.state == rWAIT) ||
          (ossptr->procs[i].request.state == rBLOCK))
      {

        ossptr->procs[i].request.state = rDENY;
      }
    }
    ossSemPost();

    //wait for all processes to finish
    while (report.pRun.curr > 0){
      ossWaitchild();
    }
  }


  printReport();
  deallocateSHM();

  exit(EXIT_SUCCESS);
}

//Find an unused process
static int emptyProc(struct process *procs){
  int i;
  for (i = 0; i < processSize; i++){
    if (procs[i].pid == 0){
      return i;
    }
  }
  return -1;
}

//Spawn a user program
static int startProcess(){
  char argID[5];

  const int idx = emptyProc(ossptr->procs);
  if (idx == -1){
    return -1;
  }
  struct process *proc = &ossptr->procs[idx];

  snprintf(argID, 5, "%d", idx);

  const pid_t pid = fork();
  if (pid == -1){
    fprintf(stderr,"%s: ",prog);
    perror("fork");
    return -1;
  }

  //child
  if (pid == 0){
    execl("./user_proc", "./user_proc", argID, NULL);
    fprintf(stderr,"%s: ",prog);
    perror("execl");
    exit(EXIT_FAILURE);

  }else{
    proc->pid = pid;
    proc->id = report.pStart.curr++;
    proc->processState = pREADY;

    ++report.pRun.curr;

    if(optionVerbose){
      printf("Master generates process with PID %d at time %lu:%06d\n", proc->id, ossptr->time.tv_sec, ossptr->time.tv_usec);
      linesCount++;
    }
  }

  return 0;
}

//Attach the ossptr in shared memory
static int ossAttach(){
  const unsigned short unlocked = 1;
	shmID = shmget(key_shm, sizeof(struct oss), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
	if(shmID < 0){
    fprintf(stderr,"%s: failed to get id for shared memory. ",prog);
    perror("Error");
    return -1;
  }

	semID = semget(key_sem, 1, IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
	if(semID < 0){
		fprintf(stderr,"%s: failed to get id for semaphore. ",prog);
    perror("Error");
    return -1;
	}
	ossptr = (struct oss *)shmat(shmID, NULL, 0);
	if(ossptr == (void *)-1){
    fprintf(stderr,"%s: failed to get pointer for shared memory. ",prog);
    perror("Error");
    return -1;
	}
	
	//clear the memory
	bzero(ossptr, sizeof(struct oss));

	//unlock semaphore
	if (semctl(semID, 0, SETVAL, unlocked) == -1){
    fprintf(stderr,"%s: ",prog);
    perror("semctl");
    return -1;
  }

  return 0;
}
static void sigHandler(const int sig){

  switch (sig){
  case SIGTERM:
  case SIGINT:
    printf("Master TERM/INT at time %lu:%06d\n", ossptr->time.tv_sec, ossptr->time.tv_usec);
    aliveFlag = 0; //stop master loop
    break;

  case SIGALRM:
    printf("Master ALRM at time %lu:%06d\n", ossptr->time.tv_sec, ossptr->time.tv_usec);
    aliveFlag = 0;
    break;

  case SIGCHLD:
    ossWaitchild();
    break;

  default:
    fprintf(stderr, "Error: Unknown signal received\n");
    break;
  }
}
static void increAccept(){
  acceptCount++;
  if(optionVerbose && (acceptCount % 20 == 0)){
    listSystemResources();
    fflush(stdout);
  }
}
//Unblock a process, and change its state
static int unblock(const int b, const enum requestState new_state){

  const int id = dequeueBlocked(b);
  const int idx = idToIndex(id);
  struct process *proc = &ossptr->procs[idx];
  struct timeval tv1, tv2;
  ossSemWait();

  //update process and request states
  proc->processState = pREADY;
  proc->request.state = new_state;

  if (proc->request.state == rDENY){
    denyCount++;
    if(optionVerbose){
      printf("Master denies process with PID %d waiting on R%d=%d at time %lu:%06d\n",
          proc->id, proc->request.id, proc->request.val, ossptr->time.tv_sec, ossptr->time.tv_usec);
      linesCount++;
    }
  }else{
    if(optionVerbose){
      printf("Master unblocks process with PID %d waiting on R%d=%d at time %lu:%06d\n",
        proc->id, proc->request.id, proc->request.val, ossptr->time.tv_sec, ossptr->time.tv_usec);
      linesCount++;
    }
    report.waitAccept++;//the request is accepted after waiting
    increAccept();

    //remove requested amount from system resources
    ossptr->desc[proc->request.id].val -= proc->request.val;
    proc->desc[proc->request.id].val += proc->request.val;
    proc->desc[proc->request.id].max -= proc->request.val;
  }

  timersub(&ossptr->time,&proc->waitTime,&tv1);
  timeradd(&report.waitTime, &tv1, &tv2);
  report.waitTime = tv2;
  proc->waitTime.tv_sec = 0;
  proc->waitTime.tv_usec = 0;

  ossSemPost();

  return 0;
}

//try to unblock processes in blocked queue
static int wakeupProc(){
  int i, n = 0;

  for (i = 0; i < blockedLength; i++){

    const int id = blocked[i];
    const int idx = idToIndex(id);
    struct process *proc = &ossptr->procs[idx];
    if(ossptr != NULL && proc->request.id != -1){
      if (ossptr->desc[proc->request.id].val >= proc->request.val){
        //if we have enough resource
        unblock(i, rACCEPT);
        n++; //one more unblocked process
      }
    }else{
      break;
    }
  }

  if (n == 0){ 
    //if nobody was unblocked
    //every process is in the queue
    if ((blockedLength > 0) &&
        (blockedLength == report.pRun.curr)){

      //unblock first to avoid deadlock
      unblock(0, rDENY);
    }else{
      return 0; //nobody to unblock
    }
  }

  return 1;
}

//List the processes in a deadlock
static int listDeadlocked(const int pdone[processSize]){

  int i, have_deadlocked = 0;
  for (i = 0; i < processSize; i++){
    if (!pdone[i]){ 
      //if process is not finished / done
      have_deadlocked = 1;
      break;
    }
  }

  if (have_deadlocked > 0){
    printf("\tProcesses ");
    for (i = 0; i < processSize; i++){
      if (!pdone[i]){
        printf("P%d ", ossptr->procs[i].id);
      }
    }
    printf("deadlocked.\n");
    linesCount++;
  }

  return have_deadlocked;
}

//Test for a deadlock for resource descriptors
static int detectDeadlock(){
  report.deadlockRun++;
  int i, j, avail[descriptorCount], pdone[processSize];

  //at start all procs are not done
  for (i = 0; i < processSize; i++){
    pdone[i] = 0;
  }

  //sum the available resources
  for (i = 0; i < descriptorCount; i++){
    avail[i] = ossptr->desc[i].val;
  }

  i = 0;
  while (i != processSize){

    for (i = 0; i < processSize; i++){

      struct process *proc = &ossptr->procs[i];
      if (proc->pid == 0){
        pdone[i] = 1;
      }

      if (pdone[i] == 1){
        continue;
      }

      //check if process can finish
      int can_complete = 1;
      for (j = 0; j < descriptorCount; j++){
        //if process need is more than system available resource
        if (ossptr->desc[j].val < (proc->desc[j].max - proc->desc[j].val)){
          can_complete = 0; //can't complete
          break;
        }
      }

      //if user is releasing or request is accepted or can complete
      if ((proc->request.val < 0) ||
          (proc->request.state != rWAIT) ||
          can_complete){

        //mark it as complete
        pdone[i] = 1;

        //add its resource to available
        for (j = 0; j < descriptorCount; j++){
          avail[j] += proc->desc[j].val;
        }
        break;
      }
    }
  }

  //check if we have a deadlock
  return listDeadlocked(pdone);
}

static int onReq(struct process *proc){

  //process the request
  requestCount++;

  if (proc->request.val < 0){ 
    //if user wants to release

    proc->request.state = rACCEPT; //request is accepted
    //return resource to system
    ossptr->desc[proc->request.id].val += -1 * proc->request.val;
    proc->desc[proc->request.id].val += proc->request.val;
    if(optionVerbose){
      printf("Master has acknowledged process P%d releasing R%d:%d at time %lu:%06d\n",
            proc->id, proc->request.id, -1 * proc->request.val, ossptr->time.tv_sec, ossptr->time.tv_usec);
      linesCount++;
    }

    
  }else if (proc->request.id == -1){
    //id=-1, means user is returning all of its resources
    int i;

    proc->request.state = rACCEPT;

    if(optionVerbose){
      printf("Master has acknowledged process P%d releasing all resources: ", proc->id);

      for (i = 0; i < descriptorCount; i++){
        if (proc->desc[i].val > 0){
          printf("R%d:%d ", i, proc->desc[i].val);

          //return resource to system
          ossptr->desc[i].val += proc->desc[i].val;
          proc->desc[i].val = 0;
        }
      }
      printf("\n");
      linesCount++;
    }

    for (i = 0; i < descriptorCount; i++){
      if (proc->desc[i].val > 0){
        //return resource to system
        ossptr->desc[i].val += proc->desc[i].val;
        proc->desc[i].val = 0;
      }
    }
  }else{
    //user wants to request a resource
    printf("Master has detected process P%d request R%d at time %lu:%06d\n", proc->id, proc->request.id,
             ossptr->time.tv_sec, ossptr->time.tv_usec);
    printf("Master running deadlock detection at time %lu:%06d\n", ossptr->time.tv_sec, ossptr->time.tv_usec);
    linesCount+=2;

    //check if we are in deadlock, after request
    if (detectDeadlock()){

      printf("\tUnsafe state after granting request; request not granted\n");
      linesCount++;
      if (enqueueBlocked(proc->id) == 0){
        proc->waitTime = ossptr->time;
        proc->request.state = rBLOCK;

        if(optionVerbose){
          printf("\tP%d added to wait queue, waiting on R%d=%d\n", proc->id, proc->request.id, proc->request.val);
          linesCount++;
        }
      }else{
        proc->request.state = rDENY;
        denyCount++;
        if(optionVerbose){
          printf("\tP%d wans't added to wait queue, queue is full\n", proc->id);
          linesCount++;
        }
      }

    }else{ //no deadlock, we accept

      printf("\tSafe state after granting request\n");
      printf("\tMaster granting P%d request R%d:%d at time %lu:%06d\n", proc->id, proc->request.id, proc->request.val,
             ossptr->time.tv_sec, ossptr->time.tv_usec);
      linesCount += 2;
      report.immediateAccept++; //the request is granted immediately
      //update system and user resource descriptors
      ossptr->desc[proc->request.id].val -= proc->request.val;
      proc->desc[proc->request.id].val += proc->request.val;

      //reduce user resource need
      proc->desc[proc->request.id].max -= proc->request.val;

      proc->request.state = rACCEPT;
      increAccept();
    }
  }
  return 0;
}

static int execReq(){
  int i;
  sigset_t mask, oldmask;
  struct timeval tv;

  //block child signals, while we process requests
  sigemptyset(&mask);
  sigaddset(&mask, SIGCHLD);
  sigprocmask(SIG_BLOCK, &mask, &oldmask);

  //lock the shared region
  ossSemWait();

  for (i = 0; i < processSize; i++){

    struct process *proc = &ossptr->procs[i];
    //if user have made a request
    if (proc->request.state == rWAIT){
      onReq(proc);
    }
  }
  ossSemPost();

  //allow the child signals, again
  sigprocmask(SIG_SETMASK, &oldmask, NULL);

  //update the timer, with dispatch time
  tv.tv_sec = 0;
  tv.tv_usec = rand() % 100;
  if (ossTimeUpdate(&tv) < 0){
    return -1;
  }

  return 0;
}

int main(const int argc, char *const argv[]){
  prog = argv[0];
  struct timeval next_start = {.tv_sec = 1, .tv_usec = 5000};
  srand(getpid());
  atexit(ossExit);

  initReport();
  
  stdout = freopen(logfile, "w", stdout);

  if (ossAttach() < 0){
    ossExit();
  } 

  //set up our clock step at 100
  clockTick.tv_sec = 0;
  clockTick.tv_usec = 100;

  signal(SIGINT, sigHandler);
  signal(SIGTERM, sigHandler);
  signal(SIGCHLD, sigHandler);
  signal(SIGALRM, sigHandler);

  alarm(maxRuntime);

  //allocate and setup resource descriptors
  descriptorsInit(ossptr->desc);

  //loop until all processes are done
  while (aliveFlag){

    //update our simulated clock
    if (ossTimeUpdate(&clockTick) < 0){
      break;
    }

    //if its time to start a new process
    if (timercmp(&ossptr->time, &next_start, >=) != 0){

      if (report.pStart.curr < report.pStart.max){
        startProcess();

        //set time for next process
        next_start.tv_sec = ossptr->time.tv_sec;
        next_start.tv_usec = ossptr->time.tv_usec + (rand() % 5000);
      }
    }

    
    execReq(); //start checking request from child
    //check the blocked processes
    wakeupProc();


    if (linesCount > lineLimit){
      //directing output to another file
      stdout = freopen("/dev/null", "w", stdout);
      linesCount = 0;
    }
  }

  return 0;
}
