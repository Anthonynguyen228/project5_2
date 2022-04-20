/*
CS_4760
Anthony Nguyen
04/19/2022
*/

#include <sys/time.h>
#include "config.h"


// For sharing memory and semaphores between enviornment
const key_t key_shm = 4506;
const key_t key_sem = 6708;


//process states
enum states { pREADY=1, pBLOCK, stTERM};

//request states
enum requestState { rACCEPT=0, rBLOCK, rWAIT, rDENY};

struct descriptor {
	int shareable;
	int max;		//maximum value
	int val;	//currently available
};
struct pDescriptor{
	int curr; //current number of processes
	int max; //maximum number of processes
};
struct descriptorRequest {
	int id;					
	int val;				
	enum requestState state;
};

struct process {
	int	pid, id;	//process and OSS ID
	struct timeval waitTime;
	enum states processState;
	struct descriptorRequest request;	//current process request

	struct descriptor desc[descriptorCount];	//process resource descriptors
};

struct oss {
	struct timeval time;
	struct process procs[processSize];

	struct descriptor desc[descriptorCount];	//system resource descriptors
	int terminateFlag;
};
struct ossReport{
	struct pDescriptor pStart, pRun, pDone;
	struct timeval waitTime;

	unsigned int immediateAccept;
	unsigned int waitAccept;
	unsigned int deadlockRun;

};
