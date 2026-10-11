//---------------------------------------------------------------------------
//
// Heap.cpp -- This file contains the definition for the Base HEAP
//		 			Manager Class.  The Base HEAP manager creates,
//		 			manages and destroys block of memory using Win32
//		 			Virtual memory calls.h
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//---------------------------------------------------------------------------
// Include Files
#ifndef HEAP_H
#include"heap.h"
#endif

#ifndef FILE_H
#include"file.h"
#endif

#include"platform_windows.h"

#include<gameos.hpp>


#include"platform_str.h" 
#include <ctype.h> // toupper
#include <bit>

// NS3 boundary: the engine system allocator belongs to the heap subsystem
// (this TU), not to whatever game/tool main links it. extern'd for all
// consumers in heap.h. Previously redefined in every main.
UserHeapPtr systemHeap = nullptr;

//---------------------------------------------------------------------------
// Static Globals
static const char CorruptMsg[] = "Heap check failed.\n";
static const char pformat[] = "%s %s\n";

GlobalHeapRec HeapList::heapRecords[MAX_HEAPS];
HeapListPtr globalHeapList = nullptr;

unsigned long memCoreLeft = 0;
unsigned long memTotalLeft = 0;

unsigned long totalSize = 0;
unsigned long totalCoreLeft = 0;
unsigned long totalLeft = 0;

bool HeapList::heapInstrumented = 0;


//---------------------------------------------------------------------------
// Macro definitions
#define USE_BEST_FIT

#ifdef _DEBUG
#define SAFE_HEAP
#endif

#ifdef _DEBUG
#define CHECK_HEAP
#endif

#ifdef LAB_ONLY
#define CHECK_HEAP
#endif

//---------------------------------------------------------------------------
// Class HeapManager Member Functions
HeapManager::~HeapManager (void)
{
	destroy();
}
		
//---------------------------------------------------------------------------
void HeapManager::destroy (void)
{
	long result = 0;
	
	//-----------------------------
	// Remove this from the UEBER HEAP 
#ifdef CHECK_HEAP
	globalHeapList->removeHeap(this);
#endif
	
	if (committedSize)
	{
		result = VirtualFree(heap,totalSize,MEM_DECOMMIT);
		if (result == FALSE)
			result = GetLastError();
	}

	if (totalSize && memReserved && heap)
	{                 
		result = VirtualFree(heap, totalSize, MEM_RELEASE);
		if (result == FALSE)
			result = GetLastError();
	}

	init();
}
		
//---------------------------------------------------------------------------
HeapManager::HeapManager (void)
{
	init();
}
		
//---------------------------------------------------------------------------
void HeapManager::init (void)
{
	heap = nullptr;
	memReserved = FALSE;
	totalSize = 0;
	committedSize = 0;
	nxt = nullptr;
}
		
//---------------------------------------------------------------------------
HeapManager::operator MemoryPtr (void)
{
	return getHeapPtr();
}
		
//---------------------------------------------------------------------------
MemoryPtr HeapManager::getHeapPtr (void)
{
	if (memReserved && totalSize && committedSize && heap)
		return heap;

	return nullptr;
}

//---------------------------------------------------------------------------
long HeapManager::createHeap (unsigned long memSize)
{
	heap = (MemoryPtr)VirtualAlloc(nullptr,memSize,MEM_RESERVE,PAGE_READWRITE);

	if (heap)
	{
		memReserved = TRUE;
		totalSize = memSize;
		return NO_ERR;
	}

	return COULDNT_CREATE;
}

//---------------------------------------------------------------------------
long HeapManager::commitHeap (unsigned long commitSize)
{
	if (commitSize == 0)
		commitSize = totalSize;
	
	if (commitSize > totalSize)
		return ALLOC_TOO_BIG;

	if (commitSize < totalSize)
		return COULDNT_COMMIT;

	unsigned long memLeft = totalSize - committedSize;
	
	if (!memLeft)
	{
		return OUT_OF_MEMORY;
	}

	if (memLeft < commitSize)
	{
		commitSize = memLeft;
	}

	MemoryPtr result = (MemoryPtr)VirtualAlloc(heap,commitSize,MEM_COMMIT,PAGE_READWRITE);

	if (result == heap)
	{
		long actualSize = commitSize;
		committedSize += actualSize;

		#ifdef CHECK_HEAP		
		//-----------------------------
		// Add this to the UEBER HEAP 
		globalHeapList->addHeap(this);
		#endif
		
		//------------------------------
		// Store off who called this.
		// If this was a UserHeap,
		// the UserHeap class will
		// do its own unwind.
		uintptr_t currentEbp = 0;
		unsigned long prevEbp = 0;
		unsigned long retAddr = 0;

#ifdef PLATFORM_WINDOWS
#ifndef _WIN64
		__asm { mov currentEbp,esp }
#else	
		return NO_ERR;
#endif
#else
		// only correct for 64bit?
        // currentEbp = esp;
        asm("mov %%rsp, %0;"
            : "=r"(currentEbp)
            :
            :
        );
#endif
		prevEbp = *std::bit_cast<unsigned long *>(currentEbp);
		retAddr = *std::bit_cast<unsigned long *>(currentEbp + 4);
		whoMadeMe = retAddr;

		return NO_ERR;
	}

	return COULDNT_COMMIT;
}
		
//---------------------------------------------------------------------------
long HeapManager::decommitHeap (unsigned long decommitSize)
{
	if (decommitSize == 0 || decommitSize > committedSize)
		decommitSize = committedSize;

	if (decommitSize == 0)
		return NO_ERR;

	// The decommitted range is the tail of the committed span: [heap + newCommitted, +decommitSize).
	unsigned long newCommittedSize = committedSize - decommitSize;

	if (!VirtualFree(heap + newCommittedSize, decommitSize, MEM_DECOMMIT))
		return static_cast<long>(GetLastError());

	committedSize = newCommittedSize;
	return NO_ERR;
}


/////////////////////////////////////////////////////////////

//---------------------------------------------------------------------------
// Class UserHeap Member Functions
UserHeap::UserHeap (void) : HeapManager()
{
	heapStart = nullptr;
	heapEnd = nullptr;
	firstNearBlock = nullptr;
	heapSize = 0;
	
	#ifdef CHECK_HEAP
	mallocFatals = TRUE;
	#else
	mallocFatals = FALSE;
	#endif	
		
	heapState = NO_ERR;
}

//---------------------------------------------------------------------------
long UserHeap::init (unsigned long memSize, const char *heapId, bool useGOS)
{
	// The gos heap keeps its own name; heapName stays NULL (it used to be allocated here and then overwritten
	// with NULL below, leaking the copy).
	{
		gosHeap = gos_CreateMemoryHeap(heapId,memSize);
		useGOSGuardPage = true;

		heapStart = nullptr;
		heapEnd = nullptr;
		firstNearBlock = nullptr;
		heapSize = 0;

		heapName = nullptr;
		heapState = NO_ERR;

		#ifdef _DEBUG
		recordArray = nullptr;
		#endif
	}
	
	return NO_ERR;
}
	
#ifdef _DEBUG
//---------------------------------------------------------------------------
void UserHeap::startHeapMallocLog (void)
{
	if (!recordArray)
	{
		recordArray = new memRecord[NUMMEMRECORDS];
		memset(recordArray, 0, sizeof(memRecord) * NUMMEMRECORDS);
		recordCount = 0;
	}
	
	logMallocs = TRUE;
}	

//---------------------------------------------------------------------------
void UserHeap::stopHeapMallocLog (void)
{
	logMallocs = FALSE;
}	

//---------------------------------------------------------------------------
void UserHeap::dumpRecordLog (void)
{
}	
#endif
	
//---------------------------------------------------------------------------
UserHeap::~UserHeap (void)
{
	destroy();
}

//---------------------------------------------------------------------------
void UserHeap::destroy (void)
{
	HeapManager::destroy();
	{
		gos_DestroyMemoryHeap(gosHeap,false);
		gosHeap = nullptr;
	}
}
		
//---------------------------------------------------------------------------
unsigned long UserHeap::totalCoreLeft (void)
{
	unsigned long result = 0;

	if (gosHeap)
		return result;

	return(result);
}

//---------------------------------------------------------------------------
unsigned long UserHeap::coreLeft (void)
{
	unsigned long result = 0;

	if (gosHeap)
		return result;

	return(result);
}			

//---------------------------------------------------------------------------
void * UserHeap::Malloc (size_t memSize)
{
	void * result = nullptr;
	if (gosHeap)
	{
		gos_PushCurrentHeap( gosHeap );
		result = gos_Malloc(memSize);
		gos_PopCurrentHeap();

		return result;
	}
	return(result);
}

//---------------------------------------------------------------------------
long UserHeap::Free (void *memBlock)
{
	if (gosHeap)
	{
		gos_PushCurrentHeap( gosHeap );
		gos_Free(memBlock);
		gos_PopCurrentHeap();

		return 0;
	}

	long result = 0;

	return(result);
}

//---------------------------------------------------------------------------
void * UserHeap::calloc (size_t memSize)
{
	void * result = malloc(memSize);
	memset(result,0,memSize);

	return result;
}
		
//---------------------------------------------------------------------------
void UserHeap::walkHeap (bool printIt, bool skipAllocated)
{
	if (gosHeap)
	{
		gos_WalkMemoryHeap(gosHeap);
		return;
	}
}

//---------------------------------------------------------------------------
long UserHeap::getLastError (void)
{
	return heapState;
}
		
//---------------------------------------------------------------------------
void HeapList::addHeap (HeapManagerPtr newHeap)
{
	for (long i=0;i<MAX_HEAPS;i++)
	{
		if (heapRecords[i].thisHeap == nullptr)
		{
			heapRecords[i].thisHeap = newHeap;
			heapRecords[i].heapSize = newHeap->tSize();
			return;
		}
	}
}

//---------------------------------------------------------------------------
void HeapList::removeHeap (HeapManagerPtr oldHeap)
{
	for (long i=0;i<MAX_HEAPS;i++)
	{
		if (heapRecords[i].thisHeap == oldHeap)
		{
			heapRecords[i].thisHeap = nullptr;
			heapRecords[i].heapSize = 0;
			return;
		}
	}
}

void HeapList::initializeStatistics()
{
	if (heapInstrumented == 0)
	{
			StatisticFormat( "" );
			StatisticFormat( "MechCommander 2 Heaps" );
			StatisticFormat( "======================" );
			StatisticFormat( "" );

			AddStatistic("Total Memory","bytes",gos_DWORD, &(totalSize), Stat_AutoReset | Stat_Total);

			AddStatistic("Total Memory Core Left","bytes",gos_DWORD, &(totalCoreLeft), Stat_AutoReset | Stat_Total);

			AddStatistic("Total Memory Left","bytes",gos_DWORD, &(totalLeft), Stat_AutoReset | Stat_Total);

			StatisticFormat( "" );
			StatisticFormat( "" );
	
			for (long i=0;i<50;i++)
			{
				char heapString[255];
				sprintf(heapString,"Heap %d - HeapSize",i);
				AddStatistic(heapString,"bytes",gos_DWORD, &(heapRecords[i].heapSize), Stat_AutoReset | Stat_Total);

				sprintf(heapString,"Heap %d - TotalLeft",i);
				AddStatistic(heapString,"bytes",gos_DWORD, &(heapRecords[i].totalCoreLeft), Stat_AutoReset | Stat_Total);

				sprintf(heapString,"Heap %d - CoreLeft",i);
				AddStatistic(heapString,"bytes",gos_DWORD, &(heapRecords[i].coreLeft), Stat_AutoReset | Stat_Total);

				StatisticFormat( "" );
			}

		heapInstrumented = true;
	}
}
	
//---------------------------------------------------------------------------
void HeapList::update (void)
{
	totalSize = totalCoreLeft = totalLeft = 0;
	for (long i=0;i<50;i++)
	{
		if (heapRecords[i].thisHeap && (heapRecords[i].thisHeap->heapType() == USER_HEAP))
		{
			heapRecords[i].heapSize = ((UserHeapPtr)heapRecords[i].thisHeap)->tSize();
			totalSize += heapRecords[i].heapSize;

			heapRecords[i].coreLeft = ((UserHeapPtr)heapRecords[i].thisHeap)->coreLeft();
			totalLeft += heapRecords[i].coreLeft;

			heapRecords[i].totalCoreLeft = ((UserHeapPtr)heapRecords[i].thisHeap)->totalCoreLeft();
			totalCoreLeft += heapRecords[i].totalCoreLeft;
		}
		else if (heapRecords[i].thisHeap)
		{
			heapRecords[i].heapSize = heapRecords[i].thisHeap->tSize();
			totalSize += heapRecords[i].heapSize;

			heapRecords[i].coreLeft = 0;
			heapRecords[i].totalCoreLeft = 0;
		}
	}
}

//---------------------------------------------------------------------------
unsigned long textToLong (char *num)
{
	long result = 0;
	char *hexOffset = num;
	
	hexOffset += 2;
	long numDigits = static_cast<long>(strlen(hexOffset)-1);
	long power = 0;
	
	for (long count = numDigits;count >= 0;count--,power++)
	{
		unsigned char currentDigit = toupper(hexOffset[count]);
		
		if (currentDigit >= 'A' && currentDigit <= 'F')
		{
			result += (currentDigit - 'A' + 10)<<(4*power);
		}
		else if (currentDigit >= '0' && currentDigit <= '9')
		{
			result += (currentDigit - '0')<<(4*power);
		}
		else
		{
			//---------------------------------------------------------
			// There is a digit in here I don't understand.  Return 0.
			result = 0;
			break;
		}
	}

	return(result);
}

//-----------------------------------------------------------
long longToText (char *result, long num, unsigned long bufLen)
{
	char temp[250];
	sprintf(temp,"%08X",num);

	unsigned long numLength = static_cast<unsigned long>(strlen(temp));
	if (numLength >= bufLen)
		return(0);

	strncpy(result,temp,numLength);
	result[numLength] = '\0';
	
	return(NO_ERR);
}	

//--------------------------------------------------------------------------
long getStringFromMap (File &mapFile, unsigned long addr, char *result)
{
	//----------------------------------------
	// Convert function address to raw offset
	#ifdef TERRAINEDIT
	unsigned long offsetAdd = 0x00601000;
	#else
	unsigned long offsetAdd = 0x00601000;
	#endif
	
	unsigned long function = addr;
	function -= offsetAdd;
	
	char actualAddr[10];
	longToText(actualAddr,function,9);
	
	//------------------------------------
	// Find the first code entry address.
	// This is the first line encountered with "  Address" as the first nine characters.
	char mapFileLine[512];
	
	mapFile.seek(0);
	mapFile.readLine((MemoryPtr)mapFileLine,511);
	while (strstr(mapFileLine,"  Address") == nullptr)
	{
		mapFile.readLine((MemoryPtr)mapFileLine,511);
	}
	
	mapFile.readLine((MemoryPtr)mapFileLine,511);
	mapFile.readLine((MemoryPtr)mapFileLine,511);
	//-------------------------------------------------------------
	// We've found the first code entry.  Now, scan until
	// the current address is greater than the address asked for.
	// The previous function name is the function in question.
	char *currentAddress = &(mapFileLine[6]);
	char previousAddress[511] = { 0 };
	strncpy(previousAddress,&(mapFileLine[6]),510);
	
	while (strstr(mapFileLine,"0001:") != nullptr)
	{
		if (S_strnicmp(currentAddress,actualAddr,8) > 0)
		{
			//-----------------------------------------------
			// We've found it, print the previous address.
			strncpy(result,previousAddress,510);
			return static_cast<long>(strlen(result));
		}
		
		strncpy(previousAddress,&(mapFileLine[6]),510);
		mapFile.readLine((MemoryPtr)mapFileLine,511);
	}
	
	return(0);
}
		
//---------------------------------------------------------------------------
void HeapList::dumpLog (void)
{
	//----------------------------------------------
	// This function dumps information on each heap
	// to a log file.
	File logFile;
	logFile.create("heap.dump.log");
	
	File mapFile;
	
	long mapResult = 0;
	#ifdef _DEBUG
	#ifdef TERRAINEDIT
	mapResult = mapFile.open("teditor.map");
	#else
	mapResult = mapFile.open("mechcmdrdbg.map");
	#endif
	#endif	

	HeapManagerPtr currentHeap = nullptr;
	unsigned long heapNumber = 1;
	unsigned long mapStringSize = 0;
	char msg[1024];
	char mapInfo[513];

	unsigned long totalCommit = 0;
	unsigned long totalFree = 0;
	
	for (long i=0;i<MAX_HEAPS;i++)
	{
		currentHeap = heapRecords[i].thisHeap;
		
		if (currentHeap)
		{
			sprintf(msg,"ListNo: %d     Heap: %d     Type: %d     Made by: %08ld",i,heapNumber,currentHeap->heapType(),currentHeap->owner());
			logFile.writeLine(msg);

			if (mapResult == NO_ERR)
			{
				mapStringSize = getStringFromMap(mapFile,currentHeap->owner(),mapInfo);
				if (mapStringSize)
				{
					sprintf(msg,"Made in Function : %s",mapInfo);
					logFile.writeLine(msg);
				}
			}
			
			sprintf(msg,"HeapSize: %d     HeapStart: %08p",currentHeap->tSize(),currentHeap->getHeapPtr());
			logFile.writeLine(msg);

			totalCommit += currentHeap->tSize();
			
			if (currentHeap->heapType() == 1)
			{
				UserHeapPtr userHeap = (UserHeapPtr)currentHeap;
				sprintf(msg,"TotalCoreLeft: %d     CoreLeft: %d",userHeap->totalCoreLeft(),userHeap->coreLeft());
				logFile.writeLine(msg);
				
				sprintf(msg,"Frag Level: %f       PercentFree: %f",(float(userHeap->coreLeft())/float(userHeap->totalCoreLeft())),1.0 - (float(currentHeap->tSize()-userHeap->coreLeft())/float(currentHeap->tSize())) );
				logFile.writeLine(msg);
				
				totalFree += userHeap->coreLeft();
			}
			
			currentHeap = currentHeap->nxt;
			heapNumber++;
		}
		else
		{
			sprintf(msg,"ListNo: %d  is Freed",i);
			logFile.writeLine(msg);
		}
		
		sprintf(msg,"---------------------------");
		logFile.writeLine(msg);
	}	
	
	sprintf(msg,"Total Committed Memory: %d      Total Free in Commit: %d",totalCommit,totalFree);
	logFile.writeLine(msg);
	sprintf(msg,"---------------------------");
	logFile.writeLine(msg);
		
	logFile.close();
}

bool UserHeap::pointerOnHeap (void *ptr)
{
	return true;
}

//---------------------------------------------------------------------------
//
// Edit Log
//
//---------------------------------------------------------------------------

