// ABL native bindings: ABL host callbacks (heaps, files, fatal/endless-state) and the arg-guard repro hooks.
// Declared in ablmc2_internal.h; registered, in order, by initABL in ablmc2.cpp.
#include "ablmc2_internal.h"


void* ablSystemMallocCallback (unsigned long memSize) {

	return(systemHeap->Malloc(memSize));
}

void* ablStackMallocCallback (unsigned long memSize) {

	if (!AblSymbolHeap)
		Fatal(0, " ablSymbolMallocCallback: NULL heap ");
	return(AblStackHeap->Malloc(memSize));
}

void* ablCodeMallocCallback (unsigned long memSize) {

	if (!AblCodeHeap)
		Fatal(0, " ablCodeFreeCallback: NULL heap ");
	return(AblCodeHeap->Malloc(memSize));
}

void* ablSymbolMallocCallback (unsigned long memSize) {

	if (!AblSymbolHeap)
		Fatal(0, " ablSymbolMallocCallback: NULL heap ");
	return(AblSymbolHeap->Malloc(memSize));
}

void ablSystemFreeCallback (void* memBlock) {

	systemHeap->Free(memBlock);
}

void ablStackFreeCallback (void* memBlock) {

	if (!AblStackHeap)
		Fatal(0, " ablStackFreeCallback: NULL heap ");
	AblStackHeap->Free(memBlock);
}

void ablCodeFreeCallback (void* memBlock) {

	if (!AblCodeHeap)
		Fatal(0, " ablCodeFreeCallback: NULL heap ");
	AblCodeHeap->Free(memBlock);
}

void ablSymbolFreeCallback (void* memBlock) {

	if (!AblSymbolHeap)
		Fatal(0, " ablSymbolFreeCallback: NULL heap ");
	AblSymbolHeap->Free(memBlock);
}

long ablFileCreateCB (void** file, const char* fName) {

	*file = new File;
	if (*file == NULL)
		Fatal(0, " unable to create ABL file");
	if (((FilePtr)*file)->create(fName) != NO_ERR) {
		char s[256];
		sprintf(s, " ABL.ablFileOpenCB: unable to create file [%s] ", fName);
		Fatal(0, s);
	}
	return(NO_ERR);
}

long ablFileOpenCB (void** file, const char* fName) {

	*file = new File;
	if (*file == NULL)
		Fatal(0, " unable to create ABL file");

	//Filenames MUST be all lowercase or Hash won't find 'em!
    char* tmpstr = strdup(fName);
	CharLower(tmpstr);
	if (((FilePtr)*file)->open(tmpstr) != NO_ERR)
		STOP((" unable to open ABL File %s",fName));
    free(tmpstr);
	return(NO_ERR);
}

long ablFileCloseCB (void** file) {

	((FilePtr)*file)->close();

	delete (FilePtr)*file;
	*file = NULL;

	return(0);
}

bool ablFileEofCB (void* file) {

	return(((FilePtr)file)->eof());

}

long ablFileReadCB (void* file, unsigned char* buffer, long length) {

	return(((FilePtr)file)->read(buffer, length));
}

int32_t ablFileReadIntCB (void* file) {

	return(((FilePtr)file)->readInt());
}

long ablFileReadLongCB (void* file) {

	return(((FilePtr)file)->readLong());
}

long ablFileReadStringCB (void* file, unsigned char* buffer) {

	return(((FilePtr)file)->readString(buffer));
}

long ablFileReadLineExCB (void* file, unsigned char* buffer, long maxLength) {

	return(((FilePtr)file)->readLineEx(buffer, maxLength));
}

long ablFileWriteCB (void* file, unsigned char* buffer, long length) {

	return(((FilePtr)file)->write(buffer, length));
}

long ablFileWriteByteCB (void* file, unsigned char byte) {

	return(((FilePtr)file)->writeByte(byte));
}

long ablFileWriteIntCB (void* file, int32_t value) {

	return(((FilePtr)file)->writeInt(value));
}

long ablFileWriteLongCB (void* file, long value) {

	return(((FilePtr)file)->writeLong(value));
}

long ablFileWriteStringCB (void* file, const char* buffer) {

	return(((FilePtr)file)->writeString(buffer));
}

void ablDebuggerPrintCallback (const char* s) {

	//ABLDebuggerOut->print(s);
	char msg[1024];
	sprintf(msg, "%s\n", s);
	SPEW((0,msg));
}

void ablDebugPrintCallback (const char* s) {

	DEBUGWINS_print(s, 0);
}

void ablSeedRandom (unsigned long seed) {

	gos_srand(seed);
}

void ablFatalCallback (long code, const char* s) {

	// Rate-limit per-message spam (e.g. unimplemented ABL extensions firing
	// on every script tick). Fixed C array -- no heap, no STL.
	static const int kSlots    = 16;
	static const int kMaxPrints = 5;
	struct AblMsgSlot { unsigned int hash; int count; };
	static AblMsgSlot slots[kSlots];
	static int  slotCount = 0;
	static bool tableFull = false;

	// djb2-lite over first 64 chars.
	unsigned int h = 5381u;
	const char* p = s ? s : "";
	for (int i = 0; *p && i < 64; ++p, ++i)
		h = h * 33u ^ (unsigned char)*p;

	for (int i = 0; i < slotCount; ++i) {
		if (slots[i].hash == h) {
			if (++slots[i].count <= kMaxPrints)
				STOP((s));
			else if (slots[i].count == kMaxPrints + 1)
				STOP(("[ABL] suppressing further identical errors (hash %08x): %s", h, s));
			return;
		}
	}

	if (slotCount < kSlots) {
		slots[slotCount++] = { h, 1 };
		STOP((s));
	} else if (!tableFull) {
		tableFull = true;
		STOP(("[ABL] fatal callback: %d unique errors tracked; further distinct messages suppressed", kSlots));
	}
}

extern GameObjectPtr LastCoreAttackTarget;

void ablEndlessStateCallback (UserFile* log) {

	char s[256];
	sprintf(s, "Mover = %s (%d)", CurWarrior->getVehicle()->getName(), CurWarrior->getVehicle()->getPartId());
	log->write(s);
	sprintf(s, "     Status = %d", CurWarrior->getVehicle()->getStatus());
	log->write(s);
	sprintf(s, "     NumWeapons = %d", CurWarrior->getVehicle()->numWeapons);
	log->write(s);
	sprintf(s, "     NumFunctionalWeapons = %d", CurWarrior->getVehicle()->numFunctionalWeapons);
	log->write(s);
	sprintf(s, "     TacOrder = ");
	CurWarrior->getCurTacOrder()->debugString(CurWarrior, &s[16]);
	log->write(s);
	log->write(" ");
	if (LastCoreAttackTarget) {
		sprintf(s, "Target = %s (%d)", LastCoreAttackTarget->getName(), LastCoreAttackTarget->getPartId());
		log->write(s);
		Stuff::Vector3D pos;
		pos = LastCoreAttackTarget->getPosition();
		sprintf(s, "Target Position = (%.3f, %.3f, %.3f) [%d, %d]",
			pos.x, pos.y, pos.z,
			LastCoreAttackTarget->cellPositionRow,
			LastCoreAttackTarget->cellPositionCol);
		log->write(s);
		sprintf(s, "     Status = %d", LastCoreAttackTarget->getStatus());
		log->write(s);
		if (LastCoreAttackTarget->isMover()) {
			MoverPtr mover = (MoverPtr)LastCoreAttackTarget;
			sprintf(s, "     NumWeapons = %d", mover->numWeapons);
			log->write(s);
			sprintf(s, "     NumFunctionalWeapons = %d", mover->numFunctionalWeapons);
			log->write(s);
			sprintf(s, "     TacOrder = ");
			mover->getPilot()->getCurTacOrder()->debugString(CurWarrior, &s[16]);
			log->write(s);
		}
		}
	else {
		sprintf(s, "Target = NULL");
		log->write(s);
	}
}

static void runAblArgGuardRepro2A(void);

// Test-only repro hook (gate: MC2_ABL_ARG_GUARD_REPRO=1) for the null-pointer arg guard. It calls the exact
// guard check and write site directly: the ABL call path needs a compiled module, and a standalone harness
// would need ~50 TUs. A null tos->address makes ABLi_popRealPtr return exactly nullptr (the StackItem union
// puts .real at offset 0), so "if (s_ablArgGuard && !relPos)" catches it with no near-null gap.
// Guard ON (MC2_ABL_ARG_GUARD=1): [ABL_ARG_GUARD] is logged, the function returns, nothing crashes.
// Guard OFF: relPos[0] = 1.0f writes to 0x0 and crashes, which is the point; smoke always sets the guard.
void runAblArgGuardRepro(void) {
    const char* repro_env = getenv("MC2_ABL_ARG_GUARD_REPRO");
    if (!repro_env || repro_env[0] != '1')
        return;

    printf("[ABL_REPRO] ABL-BAD-NATIVE-ARG-REPRO-1 starting\n");
    fflush(stdout);

    // Simulate what ABLi_popRealPtr returns when tos->address == NULL.
    // StackItem union has .real at byte offset 0, so
    //   (float*)(&((StackItemPtr)NULL)->real) == (float*)NULL == nullptr.
    float* relPos = nullptr;

    printf("[ABL_REPRO] relPos from simulated null tos->address = %p\n", (void*)relPos);
    fflush(stdout);

    // ---- GUARD PATH (MC2_ABL_ARG_GUARD=1) ----
    if (s_ablArgGuard && !relPos) {
        // Mirrors execGetRelativePositionToObject guard lines exactly.
        abl_arg_guard_log("execGetRelativePositionToObject[REPRO]", "relPos");
        printf("[ABL_REPRO] guard fired -- no crash -- PASS\n");
        fflush(stdout);
        // Also run the 2A sibling repro (Point-variant pos + relPos cases).
        runAblArgGuardRepro2A();
        return;
    }

    // ---- CRASH PATH (MC2_ABL_ARG_GUARD unset) ----
    // If we reach here, guard is off.  The write below will segfault.
    printf("[ABL_REPRO] guard OFF -- about to write relPos[0] -> WILL CRASH\n");
    fflush(stdout);
    // Volatile prevents the compiler from eliding the null write.
    volatile float* vp = relPos;
    vp[0] = 1.0f;  // writes to 0x0 -> access violation
    // Unreachable:
    printf("[ABL_REPRO] survived null write -- UNEXPECTED (optimizer elided?)\n");
    fflush(stdout);
}

// Point-variant sibling of runAblArgGuardRepro (pos + relPos null cases); called from it once the Object-variant
// guard has returned. Guard ON: both cases log [ABL_ARG_GUARD] and skip. Guard OFF: the write to 0x0 crashes.
static void runAblArgGuardRepro2A(void) {
    const char* repro_env = getenv("MC2_ABL_ARG_GUARD_REPRO");
    if (!repro_env || repro_env[0] != '1')
        return;

    printf("[ABL_REPRO_2A] execGetRelativePositionToPoint sibling repro starting\n");
    fflush(stdout);

    // --- Case 1: null pos (the READ input) ---
    {
        float* pos = nullptr;
        float* relPos_valid = (float*)0x1; // irrelevant — pos guard fires first
        (void)relPos_valid;
        printf("[ABL_REPRO_2A] case1: pos=NULL relPos_valid=%p\n", (void*)relPos_valid);
        fflush(stdout);
        if (s_ablArgGuard && !pos) {
            abl_arg_guard_log("execGetRelativePositionToPoint[REPRO2A-pos]", "pos");
            printf("[ABL_REPRO_2A] case1 guard fired -- no crash -- PASS\n");
            fflush(stdout);
        } else {
            printf("[ABL_REPRO_2A] case1 guard OFF -- about to read pos[0] -> WILL CRASH\n");
            fflush(stdout);
            volatile float* vp = pos;
            (void)vp[0]; // read from 0x0 -> access violation
            printf("[ABL_REPRO_2A] case1 survived -- UNEXPECTED\n");
            fflush(stdout);
        }
    }

    // --- Case 2: null relPos (the WRITE output) ---
    {
        float dummy[3] = {1.0f, 2.0f, 0.0f};
        float* pos_valid = dummy;
        float* relPos = nullptr;
        printf("[ABL_REPRO_2A] case2: pos_valid=%p relPos=NULL\n", (void*)pos_valid);
        fflush(stdout);
        if (s_ablArgGuard && !relPos) {
            abl_arg_guard_log("execGetRelativePositionToPoint[REPRO2A-relPos]", "relPos");
            printf("[ABL_REPRO_2A] case2 guard fired -- no crash -- PASS\n");
            fflush(stdout);
        } else {
            printf("[ABL_REPRO_2A] case2 guard OFF -- about to write relPos[0] -> WILL CRASH\n");
            fflush(stdout);
            volatile float* vp = relPos;
            vp[0] = 1.0f; // write to 0x0 -> access violation
            printf("[ABL_REPRO_2A] case2 survived -- UNEXPECTED\n");
            fflush(stdout);
        }
    }

    printf("[ABL_REPRO_2A] done\n");
    fflush(stdout);
}
