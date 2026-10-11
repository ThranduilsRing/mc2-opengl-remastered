// Pathfinding: GlobalMap construction: area flood fill, special areas, door collection and the packet read/write.
// Declared in the class headers; shared names in move_internal.h.
#include "move_internal.h"


// Bounded readPacket helper for GlobalMap::init — refuses to read a packet
// whose unpacked size exceeds the caller's buffer. Fixes silent heap
// corruption on mod content where the pak's move-data packet layout is
// larger than the schema vanilla MC2 assumes. Returns -1 on bound failure,
// otherwise passes through the underlying readPacket result (0 = fail,
// >0 = bytes read).
static int safeReadPacket(PacketFilePtr packetFile, int packetNum,
                          unsigned char* buffer, size_t maxBytes, const char* label)
{
    if (packetFile->seekPacket(packetNum) != NO_ERR)
        return 0;
    size_t unpacked = (size_t)packetFile->getPacketSize();
    if (unpacked > maxBytes) {
        PAUSE(("GlobalMap.init: %s packet %d unpack-size %u > buffer %u — bailing pre-corruption",
               label, packetNum, (unsigned)unpacked, (unsigned)maxBytes));
        return -1;
    }
    return packetFile->readPacket(packetNum, buffer);
}

// MCO wide-schema DoorLink reader. MCO DoorLink is 13 bytes (doorIndex
// widened to int; doorSide/cost/openCost unchanged) vs MC2's 11. Reads the
// wide records into a temp buffer and widens field-by-field into the real
// narrow DoorLink array. Returns the underlying safeReadPacket result
// (>0 = ok, 0 = read fail, -1 = oversize bail).
static int readDoorLinksMCO(PacketFilePtr packetFile, int packetNum,
                            DoorLink* dst, size_t numLinks, size_t mcoStride,
                            UserHeapPtr heap)
{
    size_t tempSize = mcoStride * numLinks;
    unsigned char* tempBuf = (unsigned char*)heap->Malloc(tempSize);
    gosASSERT(tempBuf != NULL);
    int result = safeReadPacket(packetFile, packetNum, tempBuf, tempSize, "doorLinks(MCO)");
    if (result > 0) {
        for (size_t k = 0; k < numLinks; k++) {
            const unsigned char* p = tempBuf + k * mcoStride;
            int doorIndexVal, costVal, openCostVal;
            memcpy(&doorIndexVal, p + 0, sizeof(int)); // MCO doorIndex(int,0)
            char doorSideVal = (char)p[4];             // MCO doorSide(char,4)
            memcpy(&costVal,      p + 5, sizeof(int));  // MCO cost(int,5)
            memcpy(&openCostVal,  p + 9, sizeof(int));  // MCO openCost(int,9)
            gosASSERT(doorIndexVal >= -32768 && doorIndexVal <= 32767);
            dst[k].doorIndex = (short)doorIndexVal;
            dst[k].doorSide  = doorSideVal;
            dst[k].cost      = costVal;
            dst[k].openCost  = openCostVal;
        }
    }
    heap->Free(tempBuf);
    return result;
}


void* GlobalMap::operator new (size_t ourSize) {

	void* result = systemHeap->Malloc(ourSize);
	return(result);
}

void GlobalMap::operator delete (void* us) {

	systemHeap->Free(us);
}	

void GlobalMap::init (long w, long h) {

	width = w;
	height = h;
	areaMap = (short*)systemHeap->Malloc(sizeof(short) * w * h);
	gosASSERT(areaMap != NULL);
	for (long r = 0; r < height; r++)
		for (long c = 0; c < width; c++)
			areaMap[r * width + c] = -1;

	gosASSERT(((width % SECTOR_DIM) == 0) && ((height % SECTOR_DIM) == 0));
	sectorWidth = w / SECTOR_DIM;
	sectorHeight = w / SECTOR_DIM;

	numAreas = 0;
	areas = nullptr;
	numDoors = 0;
	doors = nullptr;
	doorBuildList = nullptr;
	pathExistsTable = nullptr;
	blank = false;
}

constexpr int GLOBALMAP_VERSION_NUMBER = 0x022523;	// My 51st birthday:)

long GlobalMap::init (PacketFilePtr packetFile, long whichPacket) {

	long startPacket = whichPacket;

	uint32_t version = 0;
	long result = packetFile->readPacket(whichPacket++, (unsigned char*)&version);
	if (result == 0)
		Fatal(result, " GlobalMap.init: unable to read version packet ");
	bool badVersion = false;
	if (version != GLOBALMAP_VERSION_NUMBER)
		badVersion = true;

	result = packetFile->readPacket(whichPacket++, (unsigned char*)&height);
	if (result == 0)
		Fatal(result, " GlobalMap.init: unable to read height packet ");
	result = packetFile->readPacket(whichPacket++, (unsigned char*)&width);
	if (result == 0)
		Fatal(result, " GlobalMap.init: unable to read width packet ");
	int sectorDim; // this is no longer used...
	result = packetFile->readPacket(whichPacket++, (unsigned char*)&sectorDim);
	if (result == 0)
		Fatal(result, " GlobalMap.init: unable to read sectorDim packet ");
	result = packetFile->readPacket(whichPacket++, (unsigned char*)&sectorHeight);
	if (result == 0)
		Fatal(result, " GlobalMap.init: unable to read sectorHeight packet ");
	result = packetFile->readPacket(whichPacket++, (unsigned char*)&sectorWidth);
	if (result == 0)
		Fatal(result, " GlobalMap.init: unable to read sectorWidth packet ");
	result = packetFile->readPacket(whichPacket++, (unsigned char*)&numAreas);
	if (result == 0)
		Fatal(result, " GlobalMap.init: unable to read numAreas packet ");
	result = packetFile->readPacket(whichPacket++, (unsigned char*)&numDoors);
	if (result == 0)
		Fatal(result, " GlobalMap.init: unable to read numDoors packet ");
	result = packetFile->readPacket(whichPacket++, (unsigned char*)&numDoorInfos);
	if (result == 0)
		Fatal(result, " GlobalMap.init: unable to read numDoorInfos packet ");
	result = packetFile->readPacket(whichPacket++, (unsigned char*)&numDoorLinks);
	if (result == 0)
		Fatal(result, " GlobalMap.init: unable to read numDoorLinks packet ");

	if (badVersion) {
		// Bad version map, so return number of packets but bail...
		badLoad = true;
		return(13 + numDoorInfos + (numDoors + NUM_DOOR_OFFSETS) * 2);
	}

	// MCO wide-schema detection.
	// MechCommander Gold (MCO) campaign mods (Wolf Dragoons / Clan Eagle /
	// Exodus) write the same MC2 move graph but with every `short` field
	// widened to `int` (same field order, char/bool/DWORD-ptr/trailing-int
	// unchanged). Stock + MC2X paks are byte-for-byte the MC2 narrow schema.
	// Detection is purely structural and disjoint from stock:
	//   * Stock/MC2X areaMap packet is ZLIB-stored, unpacked size == 2*h*w
	//     (sizeof(short) per cell).
	//   * MCO areaMap packet is RAW, size == 4*h*w (sizeof(int) per cell).
	// These can never collide. A confirming guard requires the areas packet
	// to unpack to exactly the MCO area stride (31 bytes) * numAreas; if that
	// does not hold we leave the flag false and fall through to the existing
	// (narrow) path — never misparse a hypothetical third variant.
	// When the flag is true, each of the five alloc/read sites below reads a
	// temp buffer at the MCO stride and widens field-by-field into the real
	// narrow MC2 arrays, so all downstream code is untouched.
	// MCO on-disk strides (#pragma pack(1)).
	const size_t MCO_AREAMAP_CELL_SIZE  = 4;   // int      (MC2: short, 2)
	const size_t MCO_AREA_SIZE          = 31;  // GlobalMapArea (MC2: 25)
	const size_t MCO_DOORINFO_SIZE      = 5;   // DoorInfo       (MC2: 3)
	const size_t MCO_DOOR_SIZE          = 73;  // GlobalMapDoor  (MC2: 57)
	const size_t MCO_DOORLINK_SIZE      = 13;  // DoorLink       (MC2: 11)

	bool mcoWideSchema = false;
	{
		packetFile->seekPacket(whichPacket);
		size_t amSize = (size_t)packetFile->getPacketSize();
		if (height > 0 && width > 0 &&
		    amSize == MCO_AREAMAP_CELL_SIZE * (size_t)height * (size_t)width)
			mcoWideSchema = true;
		// Confirming guard: the areas packet (whichPacket + 1) must unpack to
		// exactly the MCO area stride * numAreas, else this is not MCO.
		if (mcoWideSchema) {
			packetFile->seekPacket(whichPacket + 1);
			size_t areasSize = (size_t)packetFile->getPacketSize();
			if (areasSize != MCO_AREA_SIZE * (size_t)numAreas)
				mcoWideSchema = false;
		}
		packetFile->seekPacket(whichPacket);
	}

	// Mod-tolerance: Carver5O pak contains an areaMap packet of 360000 bytes
	// where vanilla schema expects 180000 (sizeof(short)*height*width for
	// baseline map dims). The mod engine may use int per cell instead of
	// short, or a different dim encoding. Rather than bail, allocate to
	// whichever is larger so the packet fits; downstream indexing uses
	// baseline (width/height) so the "extra" bytes are safe scratch.
	if (mcoWideSchema) {
		// MCO: areaMap is RAW int-per-cell. Allocate the real narrow array at
		// the MC2 size; read the wide cells into a temp buffer and narrow.
		size_t cellCount = (size_t)height * (size_t)width;
		areaMap = (short*)systemHeap->Malloc(sizeof(short) * cellCount);
		gosASSERT(areaMap != NULL);
		size_t tempSize = MCO_AREAMAP_CELL_SIZE * cellCount;
		unsigned char* tempBuf = (unsigned char*)systemHeap->Malloc(tempSize);
		gosASSERT(tempBuf != NULL);
		result = safeReadPacket(packetFile, whichPacket++, tempBuf, tempSize, "areaMap(MCO)");
		if (result > 0) {
			for (size_t c = 0; c < cellCount; c++) {
				int v;
				memcpy(&v, tempBuf + c * MCO_AREAMAP_CELL_SIZE, sizeof(int));
				gosASSERT(v >= -32768 && v <= 32767);
				areaMap[c] = (short)v;
			}
		}
		systemHeap->Free(tempBuf);
	} else {
		packetFile->seekPacket(whichPacket);
		size_t packetSize = (size_t)packetFile->getPacketSize();
		size_t expected = sizeof(short) * (size_t)height * (size_t)width;
		size_t allocSize = (packetSize > expected) ? packetSize : expected;
		areaMap = (short*)systemHeap->Malloc(allocSize);
		gosASSERT(areaMap != NULL);
		result = safeReadPacket(packetFile, whichPacket++, (unsigned char*)areaMap,
		                        allocSize, "areaMap");
	}
	if (result < 0) {
		badLoad = true;
		return(13 + numDoorInfos + (numDoors + NUM_DOOR_OFFSETS) * 2);
	}
	if (result == 0)
		Fatal(result, " GlobalMap.init: unable to read areaMap packet ");

	// Same mod-tolerance pattern for areas packet. Carver5O may also have
	// a larger areas struct size per entry (extended mod schema).
	if (mcoWideSchema) {
		// MCO GlobalMapArea is 31 bytes (shorts widened to int). Read into a
		// temp buffer at MCO stride and widen field-by-field into the real
		// narrow MC2 areas[] array.
		areas = (GlobalMapAreaPtr)systemHeap->Malloc(sizeof(GlobalMapArea) * (size_t)numAreas);
		areas_cellsCovered = (short**)systemHeap->Malloc(sizeof(short*) * numAreas);
		areas_doors = (DoorInfoPtr*)systemHeap->Malloc(sizeof(DoorInfoPtr) * numAreas);
		gosASSERT(areas != NULL);
		gosASSERT(areas_cellsCovered != NULL);
		gosASSERT(areas_doors != NULL);
		size_t tempSize = MCO_AREA_SIZE * (size_t)numAreas;
		unsigned char* tempBuf = (unsigned char*)systemHeap->Malloc(tempSize);
		gosASSERT(tempBuf != NULL);
		result = safeReadPacket(packetFile, whichPacket++, tempBuf, tempSize, "areas(MCO)");
		if (result > 0) {
			for (long i = 0; i < numAreas; i++) {
				const unsigned char* p = tempBuf + (size_t)i * MCO_AREA_SIZE;
				int sectorR, sectorC, typeVal, numDoorsVal, ownerWIDVal;
				memcpy(&sectorR,     p + 0,  sizeof(int));   // MCO sectorR(int,0)
				memcpy(&sectorC,     p + 4,  sizeof(int));   // MCO sectorC(int,4)
				// skip doors_ptr(int,8)
				memcpy(&typeVal,     p + 12, sizeof(int));   // MCO type(int,12)
				memcpy(&numDoorsVal, p + 16, sizeof(int));   // MCO numDoors(int,16)
				memcpy(&ownerWIDVal, p + 20, sizeof(int));   // MCO ownerWID(int,20)
				char teamIDVal = (char)p[24];                // MCO teamID(char,24)
				bool offMapVal  = (p[25] != 0);              // MCO offMap(bool,25)
				bool openVal    = (p[26] != 0);              // MCO open(bool,26)
				// skip cellsCovered_ptr(int,27)
				gosASSERT(sectorR     >= -32768 && sectorR     <= 32767);
				gosASSERT(sectorC     >= -32768 && sectorC     <= 32767);
				gosASSERT(numDoorsVal >= -32768 && numDoorsVal <= 32767);
				areas[i].sectorR  = (short)sectorR;
				areas[i].sectorC  = (short)sectorC;
				areas[i].type     = (AreaType)typeVal;
				areas[i].numDoors = (short)numDoorsVal;
				areas[i].ownerWID = ownerWIDVal;
				areas[i].teamID   = teamIDVal;
				areas[i].offMap   = offMapVal;
				areas[i].open     = openVal;
				areas[i].doors_Legacy32bitPtr = 0;
				areas[i].cellsCovered_Legacy32bitPtr = 0;
			}
		}
		systemHeap->Free(tempBuf);
	} else {
		packetFile->seekPacket(whichPacket);
		size_t packetSize = (size_t)packetFile->getPacketSize();
		size_t expected = sizeof(GlobalMapArea) * (size_t)numAreas;
		size_t allocSize = (packetSize > expected) ? packetSize : expected;
		areas = (GlobalMapAreaPtr)systemHeap->Malloc(allocSize);
		areas_cellsCovered = (short**)systemHeap->Malloc(sizeof(short*) * numAreas);
		areas_doors = (DoorInfoPtr*)systemHeap->Malloc(sizeof(DoorInfoPtr) * numAreas);
		gosASSERT(areas != NULL);
		gosASSERT(areas_cellsCovered != NULL);
		gosASSERT(areas_doors != NULL);
		result = safeReadPacket(packetFile, whichPacket++, (unsigned char*)areas,
		                        allocSize, "areas");
	}
	if (result < 0) {
		badLoad = true;
		return(13 + numDoorInfos + (numDoors + NUM_DOOR_OFFSETS) * 2);
	}
	if (result == 0)
		Fatal(result, " GlobalMap.init: unable to read areas packet ");

	// Mod-tolerance: sum up areas[i].numDoors first so we can allocate
	// doorInfos[] to cover the actual need. Baseline's numDoorInfos header
	// field is sometimes smaller than the per-area-door sum in mod content
	// (Carver5O: header=200, sum=257), causing the previous overflow bail
	// and downstream heap corruption in Building::update/clearPathExistsTable.
	// Grow the allocation; store the grown count into numDoorInfos so later
	// reads/writes that reference numDoorInfos match the allocated buffer.
	long actualDoorInfoSum = 0;
	for (long i = 0; i < numAreas; i++)
		actualDoorInfoSum += areas[i].numDoors;
	long doorInfoAllocCount = (actualDoorInfoSum > numDoorInfos) ? actualDoorInfoSum : numDoorInfos;
	if (actualDoorInfoSum > numDoorInfos) {
		PAUSE(("GlobalMap.init: doorInfo sum %d exceeds header numDoorInfos %d — growing buffer to %d",
			(int)actualDoorInfoSum, (int)numDoorInfos, (int)doorInfoAllocCount));
		numDoorInfos = doorInfoAllocCount;
	}
	doorInfos = (DoorInfoPtr)systemHeap->Malloc(sizeof(DoorInfo) * doorInfoAllocCount);
	gosASSERT(doorInfos != NULL);
	long curDoorInfo = 0;
	bool doorInfoOverflow = false;
	for (long i = 0; i < numAreas; i++)
		if (areas[i].numDoors) {
			// Belt-and-suspenders: still bail if somehow curDoorInfo would
			// walk past the grown allocation.
			if (curDoorInfo + areas[i].numDoors > doorInfoAllocCount) {
				PAUSE(("GlobalMap.init: doorInfo overflow at area %d (have %d alloc, would need %d) — bailing pre-corruption",
					(int)i, (int)doorInfoAllocCount, (int)(curDoorInfo + areas[i].numDoors)));
				doorInfoOverflow = true;
				break;
			}
			if (mcoWideSchema) {
				// MCO DoorInfo is 5 bytes (doorIndex widened to int). Read the
				// area's door infos into a temp buffer and widen into the real
				// narrow doorInfos[] slice.
				size_t n = (size_t)areas[i].numDoors;
				size_t tempSize = MCO_DOORINFO_SIZE * n;
				unsigned char* tempBuf = (unsigned char*)systemHeap->Malloc(tempSize);
				gosASSERT(tempBuf != NULL);
				result = safeReadPacket(packetFile, whichPacket++, tempBuf, tempSize, "doorInfos[i](MCO)");
				if (result > 0) {
					for (size_t d = 0; d < n; d++) {
						const unsigned char* p = tempBuf + d * MCO_DOORINFO_SIZE;
						int doorIndexVal;
						memcpy(&doorIndexVal, p + 0, sizeof(int)); // MCO doorIndex(int,0)
						char doorSideVal = (char)p[4];             // MCO doorSide(char,4)
						gosASSERT(doorIndexVal >= -32768 && doorIndexVal <= 32767);
						doorInfos[curDoorInfo + (long)d].doorIndex = (short)doorIndexVal;
						doorInfos[curDoorInfo + (long)d].doorSide  = doorSideVal;
					}
				}
				systemHeap->Free(tempBuf);
			} else {
			result = safeReadPacket(packetFile, whichPacket++, (unsigned char*)&doorInfos[curDoorInfo],
			                        sizeof(DoorInfo) * areas[i].numDoors, "doorInfos[i]");
			}
			if (result < 0) {
				doorInfoOverflow = true;
				break;
			}
			if (result == 0)
				Fatal(result, " GlobalMap.init: unable to read doorInfos packet ");
			curDoorInfo += areas[i].numDoors;
		}
	if (doorInfoOverflow) {
		badLoad = true;
		return(13 + numDoorInfos + (numDoors + NUM_DOOR_OFFSETS) * 2);
	}
	Assert(numDoorInfos == curDoorInfo, 0, " GlobalMap.init: bad doorInfo count ");
	if (numDoorInfos != curDoorInfo) {
		PAUSE(("GlobalMap.init: doorInfo count mismatch (mod content) — bailing as bad load"));
		badLoad = true;
		return(13 + numDoorInfos + (numDoors + NUM_DOOR_OFFSETS) * 2);
	}

	curDoorInfo = 0;
	for (long curArea = 0; curArea < numAreas; curArea++) {
		areas_doors[curArea] = &doorInfos[curDoorInfo];
		curDoorInfo += areas[curArea].numDoors;
	}

	// Mod-tolerance: grow doors allocation to packet size if the pak uses a
	// larger door struct than baseline.
	if (mcoWideSchema) {
		// MCO GlobalMapDoor is 73 bytes (8 shorts widened to int; 7 trailing
		// ints + char/bool fields unchanged; two link pointers skipped). Read
		// into a temp buffer and widen into the real narrow doors[] array.
		long doorCount = numDoors + NUM_DOOR_OFFSETS;
		doors = (GlobalMapDoorPtr)systemHeap->Malloc(sizeof(GlobalMapDoor) * (size_t)doorCount);
		doors_links = (DoorInfoLinksPtr*)systemHeap->Malloc(sizeof(DoorInfoLinksPtr) * doorCount);
		gosASSERT(doors != NULL);
		size_t tempSize = MCO_DOOR_SIZE * (size_t)doorCount;
		unsigned char* tempBuf = (unsigned char*)systemHeap->Malloc(tempSize);
		gosASSERT(tempBuf != NULL);
		result = safeReadPacket(packetFile, whichPacket++, tempBuf, tempSize, "doors(MCO)");
		if (result > 0) {
			for (long i = 0; i < doorCount; i++) {
				const unsigned char* p = tempBuf + (size_t)i * MCO_DOOR_SIZE;
				GlobalMapDoor& d = doors[i];
				int rowV, colV, area0, area1, areaCost0, areaCost1, numLinks0, numLinks1;
				memcpy(&rowV,      p + 0,  sizeof(int)); // MCO row(int,0)
				memcpy(&colV,      p + 4,  sizeof(int)); // MCO col(int,4)
				char lengthV  = (char)p[8];              // MCO length(char,8)
				bool openV    = (p[9] != 0);             // MCO open(bool,9)
				char teamIDV  = (char)p[10];             // MCO teamID(char,10)
				memcpy(&area0,     p + 11, sizeof(int)); // MCO area[0](int,11)
				memcpy(&area1,     p + 15, sizeof(int)); // MCO area[1](int,15)
				memcpy(&areaCost0, p + 19, sizeof(int)); // MCO areaCost[0](int,19)
				memcpy(&areaCost1, p + 23, sizeof(int)); // MCO areaCost[1](int,23)
				char dir0     = (char)p[27];             // MCO direction[0](char,27)
				char dir1     = (char)p[28];             // MCO direction[1](char,28)
				memcpy(&numLinks0, p + 29, sizeof(int)); // MCO numLinks[0](int,29)
				memcpy(&numLinks1, p + 33, sizeof(int)); // MCO numLinks[1](int,33)
				// skip links_ptr[2] at MCO 37 & 41 (DWORD each)
				int costV, parentV, fromAreaIdxV, gV, hPrimeV, fPrimeV;
				unsigned int flagsV;
				memcpy(&costV,        p + 45, sizeof(int)); // MCO cost(int,45)
				memcpy(&parentV,      p + 49, sizeof(int)); // MCO parent(int,49)
				memcpy(&fromAreaIdxV, p + 53, sizeof(int)); // MCO fromAreaIndex(int,53)
				memcpy(&flagsV,       p + 57, sizeof(unsigned int)); // MCO flags(int,57)
				memcpy(&gV,           p + 61, sizeof(int)); // MCO g(int,61)
				memcpy(&hPrimeV,      p + 65, sizeof(int)); // MCO hPrime(int,65)
				memcpy(&fPrimeV,      p + 69, sizeof(int)); // MCO fPrime(int,69)
				gosASSERT(rowV       >= -32768 && rowV       <= 32767);
				gosASSERT(colV       >= -32768 && colV       <= 32767);
				gosASSERT(area0      >= -32768 && area0      <= 32767);
				gosASSERT(area1      >= -32768 && area1      <= 32767);
				gosASSERT(areaCost0  >= -32768 && areaCost0  <= 32767);
				gosASSERT(areaCost1  >= -32768 && areaCost1  <= 32767);
				gosASSERT(numLinks0  >= -32768 && numLinks0  <= 32767);
				gosASSERT(numLinks1  >= -32768 && numLinks1  <= 32767);
				d.row          = (short)rowV;
				d.col          = (short)colV;
				d.length       = lengthV;
				d.open         = openV;
				d.teamID       = teamIDV;
				d.area[0]      = (short)area0;
				d.area[1]      = (short)area1;
				d.areaCost[0]  = (short)areaCost0;
				d.areaCost[1]  = (short)areaCost1;
				d.direction[0] = dir0;
				d.direction[1] = dir1;
				d.numLinks[0]  = (short)numLinks0;
				d.numLinks[1]  = (short)numLinks1;
				d.links_Legacy32bitPtr[0] = 0;
				d.links_Legacy32bitPtr[1] = 0;
				d.cost          = costV;
				d.parent        = parentV;
				d.fromAreaIndex = fromAreaIdxV;
				d.flags         = flagsV;
				d.g             = gV;
				d.hPrime        = hPrimeV;
				d.fPrime        = fPrimeV;
				// One-shot sanity: widened indices must be in range.
				gosASSERT(d.area[0] < numAreas);
				gosASSERT(d.area[1] < numAreas);
				gosASSERT(d.numLinks[0] >= 0 && d.numLinks[0] <= 30);
				gosASSERT(d.numLinks[1] >= 0 && d.numLinks[1] <= 30);
			}
		}
		systemHeap->Free(tempBuf);
	} else {
		packetFile->seekPacket(whichPacket);
		size_t packetSize = (size_t)packetFile->getPacketSize();
		size_t expected = sizeof(GlobalMapDoor) * (size_t)(numDoors + NUM_DOOR_OFFSETS);
		size_t allocSize = (packetSize > expected) ? packetSize : expected;
		doors = (GlobalMapDoorPtr)systemHeap->Malloc(allocSize);
		doors_links = (DoorInfoLinksPtr*)systemHeap->Malloc(sizeof(DoorInfoLinksPtr) * (numDoors + NUM_DOOR_OFFSETS));
		gosASSERT(doors != NULL);
		result = safeReadPacket(packetFile, whichPacket++, (unsigned char*)doors,
		                        allocSize, "doors");
	}
	if (result < 0) {
		badLoad = true;
		return(13 + numDoorInfos + (numDoors + NUM_DOOR_OFFSETS) * 2);
	}
	if (result == 0)
		Fatal(result, " GlobalMap.init: unable to read doors packet ");

	doorLinks = (DoorLinkPtr)systemHeap->Malloc(sizeof(DoorLink) * numDoorLinks);
	gosASSERT(doorLinks != NULL);
	long numLinksRead = 0;
	bool doorLinkOverflow = false;
	for (int i = 0; i < (numDoors + NUM_DOOR_OFFSETS); i++) {
		long numLinks = doors[i].numLinks[0] + NUM_EXTRA_DOOR_LINKS;
		gosASSERT(numLinks >= 2);
		// Pre-write bounds check: doors[i].numLinks comes from file, mod
		// content can make it sum past numDoorLinks and trash the heap.
		if (numLinksRead + numLinks > numDoorLinks) {
			PAUSE(("GlobalMap.init: doorLinks overflow at door %d/0 (have %d, would need %d) — bailing pre-corruption",
				i, (int)numDoorLinks, (int)(numLinksRead + numLinks)));
			doorLinkOverflow = true;
			break;
		}
		if (mcoWideSchema) {
			result = readDoorLinksMCO(packetFile, whichPacket++, &doorLinks[numLinksRead],
			                          (size_t)numLinks, MCO_DOORLINK_SIZE, systemHeap);
		} else {
		result = safeReadPacket(packetFile, whichPacket++, (unsigned char*)&doorLinks[numLinksRead],
		                        sizeof(DoorLink) * numLinks, "doorLinks[i][0]");
		}
		if (result < 0) { doorLinkOverflow = true; break; }
		if (result <= 0)
			Fatal(result, " GlobalMap.init: Unable to write doorLinks packet ");
		doors_links[i][0] = &doorLinks[numLinksRead];
		numLinksRead += numLinks;

		numLinks = doors[i].numLinks[1] + NUM_EXTRA_DOOR_LINKS;
		gosASSERT(numLinks >= 2);
		if (numLinksRead + numLinks > numDoorLinks) {
			PAUSE(("GlobalMap.init: doorLinks overflow at door %d/1 (have %d, would need %d) — bailing pre-corruption",
				i, (int)numDoorLinks, (int)(numLinksRead + numLinks)));
			doorLinkOverflow = true;
			break;
		}
		if (mcoWideSchema) {
			result = readDoorLinksMCO(packetFile, whichPacket++, &doorLinks[numLinksRead],
			                          (size_t)numLinks, MCO_DOORLINK_SIZE, systemHeap);
		} else {
		result = safeReadPacket(packetFile, whichPacket++, (unsigned char*)&doorLinks[numLinksRead],
		                        sizeof(DoorLink) * numLinks, "doorLinks[i][1]");
		}
		if (result < 0) { doorLinkOverflow = true; break; }
		if (result <= 0)
			Fatal(result, " GlobalMap.init: Unable to write doorLinks packet ");
		doors_links[i][1] = &doorLinks[numLinksRead];
		numLinksRead += numLinks;
	}
	if (doorLinkOverflow) {
		badLoad = true;
		return(13 + numDoorInfos + (numDoors + NUM_DOOR_OFFSETS) * 2);
	}
	Assert(numLinksRead == numDoorLinks, 0, " GlobalMap.init: Incorrect Links count ");
	if (numLinksRead != numDoorLinks) {
		PAUSE(("GlobalMap.init: link count mismatch (mod content) — bailing as bad load"));
		badLoad = true;
		return(13 + numDoorInfos + (numDoors + NUM_DOOR_OFFSETS) * 2);
	}

	numOffMapAreas = 0;
	for (int i = 0; i < numAreas; i++)
		if (areas[i].offMap) {
			if (numOffMapAreas < MAX_OFFMAP_AREAS)
				offMapAreas[numOffMapAreas++] = i;
			closeArea(i);
		}
		else
			openArea(i);
	pathExistsTable = (unsigned char*)systemHeap->Malloc(numAreas * (numAreas / 4 + 1));
	if (!pathExistsTable)
		STOP(("GlobalMap.init: unable to malloc pathExistsTable"));
	clearPathExistsTable();

	if (logEnabled && !blank) {
		char s[256];
		for (long i = 0; i < numDoors; i++) {
			sprintf(s, "door %05d, %s(%d), areas %d & %d", i, doors[i].open ? "opened" : "CLOSED", doors[i].teamID, doors[i].area[0], doors[i].area[1]);
			log->write(s);
			for (long side = 0; side < 2; side++) {
				sprintf(s, "     side %d", side);
				log->write(s); 
				for (long j = 0; j < doors[i].numLinks[side]; j++) {
					sprintf(s, "          link %03d, to door %05d, cost %d",
						j,
						doors_links[i][side][j].doorIndex,
						doors_links[i][side][j].cost);
					log->write(s); 
				}
			}
		}
		log->write(" "); 

		for (int i = 0; i < numAreas; i++) {
			sprintf(s, "area %05d, %s(%d)", i, areas[i].open ? "opened" : "CLOSED", areas[i].teamID);
			log->write(s); 
			if (areas[i].ownerWID > 0) {
				sprintf(s, "     ownerWID is %d", areas[i].ownerWID);
				log->write(s);
			}
			sprintf(s, "     %s", typeString[areas[i].type]);
			log->write(s);
			for (long d = 0; d < areas[i].numDoors; d++) {
                const DoorInfo& door = areas_doors[i][d];
				sprintf(s, "     door %03d is %d (%d:%d & %d) ",
					d, door.doorIndex, door.doorSide,
					doors[door.doorIndex].area[0], doors[door.doorIndex].area[1]);
				log->write(s);
			}
			
		}
		log->write(" ");
	}

	return(whichPacket - startPacket);
}

long GlobalMap::write (PacketFilePtr packetFile, long whichPacket) {


	long numDrInfos = 0;
	for (long i = 0; i < numAreas; i++)
		if (areas[i].numDoors)
			numDrInfos++;
		long numPackets = 13 + numDrInfos + (numDoors + NUM_DOOR_OFFSETS) * 2;

	if (!packetFile)
		return(numPackets);

	uint32_t version = GLOBALMAP_VERSION_NUMBER;
	long result = packetFile->writePacket(whichPacket++, version);
	if (result <= 0)
		Fatal(result, " GlobalMap.write: Unable to write version packet ");
	result = packetFile->writePacket(whichPacket++, height);
	if (result <= 0)
		Fatal(result, " GlobalMap.write: Unable to write height packet ");
	result = packetFile->writePacket(whichPacket++, width);
	if (result <= 0)
		Fatal(result, " GlobalMap.write: Unable to write width packet ");
	uint32_t sectorDim = SECTOR_DIM;
	result = packetFile->writePacket(whichPacket++, sectorDim);
	if (result <= 0)
		Fatal(result, " GlobalMap.write: Unable to write sectorDim packet ");
	result = packetFile->writePacket(whichPacket++, sectorHeight);
	if (result <= 0)
		Fatal(result, " GlobalMap.write: Unable to write sectorHeight packet ");
	result = packetFile->writePacket(whichPacket++, sectorWidth);
	if (result <= 0)
		Fatal(result, " GlobalMap.write: Unable to write sectorWidth packet ");
	result = packetFile->writePacket(whichPacket++, numAreas);
	if (result <= 0)
		Fatal(result, " GlobalMap.write: Unable to write numAreas packet ");
	result = packetFile->writePacket(whichPacket++, numDoors);
	if (result <= 0)
		Fatal(result, " GlobalMap.write: Unable to write numDoors packet ");
	result = packetFile->writePacket(whichPacket++, numDoorInfos);
	if (result <= 0)
		Fatal(result, " GlobalMap.write: Unable to write numDoorInfos packet ");
	result = packetFile->writePacket(whichPacket++, numDoorLinks);
	if (result <= 0)
		Fatal(result, " GlobalMap.write: Unable to write numDoorLinks packet ");

	result = packetFile->writePacket(whichPacket++, (unsigned char*)areaMap, sizeof(short) * height * width);
	if (result <= 0)
		Fatal(result, " GlobalMap.write: Unable to write areaMap packet ");

	DoorInfoPtr doorInfoSave[MAX_GLOBALMAP_AREAS];
	for (int i = 0; i < numAreas; i++) {
		doorInfoSave[i] = areas_doors[i];
		areas_doors[i] = nullptr;

		areas[i].ownerWID = 0;
		areas[i].teamID = -1;
		areas[i].open = true;
	}
	result = packetFile->writePacket(whichPacket++, (unsigned char*)areas, sizeof(GlobalMapArea) * numAreas);
	for (int i = 0; i < numAreas; i++) {
		areas_doors[i] = doorInfoSave[i];
	}
	if (result <= 0)
		Fatal(result, " GlobalMap.write: Unable to write areas packet ");

	long maxDoors = 0;
	for (long d = 0; d < numDoors; d++) {
		for (long s = 0; s < 2; s++) {
			long areaNumDoors = areas[doors[d].area[s]].numDoors;
			if (areaNumDoors > maxDoors)
				maxDoors = areaNumDoors;
		}
	}
	doors[numDoors + DOOR_OFFSET_START].numLinks[0] = maxDoors;
	doors[numDoors + DOOR_OFFSET_START].numLinks[1] = 0;
	doors[numDoors + DOOR_OFFSET_GOAL].numLinks[0] = maxDoors;
	doors[numDoors + DOOR_OFFSET_GOAL].numLinks[1] = 0;

	long numDoorInfosWritten = 0;
	for (int i = 0; i < numAreas; i++)
		if (areas[i].numDoors) {
			long packetSize = sizeof(DoorInfo) * areas[i].numDoors;
			//result = packetFile->writePacket(whichPacket++, (unsigned char*)areas[i].doors, packetSize);
			// STORAGE_TYPE_RAW: skip compress+decompress cycle for small per-area
			// buffers — zlib adds no benefit on buffers this small and the
			// compress2()+uncompress() round-trip in writePacket costs ~1ms each.
			// readPacket handles RAW transparently on load.
			result = packetFile->writePacket(whichPacket++, (unsigned char*)areas_doors[i], packetSize, STORAGE_TYPE_RAW);
			if (result <= 0)
				Fatal(result, " GlobalMap.write: Unable to write doorInfos packet ");
			numDoorInfosWritten += areas[i].numDoors;
		}
	Assert(numDoorInfosWritten == numDoorInfos, 0, " GlobalMap.write: Error in writing DoorInfos ");

	DoorLinkPtr doorLinkSave[MAX_GLOBALMAP_DOORS][2];
	for (int i = 0; i < numDoors + NUM_DOOR_OFFSETS; i++) {
		doorLinkSave[i][0] = doors_links[i][0];
		doorLinkSave[i][1] = doors_links[i][1];
		doors_links[i][0] = nullptr;
		doors_links[i][1] = nullptr;
		doors[i].parent = 0;
		doors[i].fromAreaIndex = 0;
		doors[i].flags = 0;
		doors[i].g = 0;
		doors[i].hPrime = 0;
		doors[i].fPrime = 0;
	}
	result = packetFile->writePacket(whichPacket++, (unsigned char*)doors, sizeof(GlobalMapDoor) * (numDoors + NUM_DOOR_OFFSETS));
	for (int i = 0; i < (numDoors + NUM_DOOR_OFFSETS); i++) {
		doors_links[i][0] = doorLinkSave[i][0];
		doors_links[i][1] = doorLinkSave[i][1];
	}
	if (result <= 0)
		Fatal(result, " GlobalMap.write: Unable to write doors packet ");

	
	long numberL = 0;
	for (int i = 0; i < (numDoors + NUM_DOOR_OFFSETS); i++) {
		long numLinks = doors[i].numLinks[0] + NUM_EXTRA_DOOR_LINKS;
		gosASSERT(numLinks >= 2);
		// STORAGE_TYPE_RAW: skip compress+decompress per door-link buffer.
		// Same rationale as doorInfos above — these are small buffers and the
		// compress2()+uncompress() overhead dominates over any size savings.
		result = packetFile->writePacket(whichPacket++, (unsigned char*)doors_links[i][0], sizeof(DoorLink) * numLinks, STORAGE_TYPE_RAW);
		if (result <= 0)
			Fatal(result, " GlobalMap.write: Unable to write doorLinks packet ");
		numberL += numLinks;

		numLinks = doors[i].numLinks[1] + NUM_EXTRA_DOOR_LINKS;
		gosASSERT(numLinks >= 2);
		result = packetFile->writePacket(whichPacket++, (unsigned char*)doors_links[i][1], sizeof(DoorLink) * numLinks, STORAGE_TYPE_RAW);
		if (result <= 0)
			Fatal(result, " GlobalMap.write: Unable to write doorLinks packet ");
		numberL += numLinks;
	}

	if (numberL != numDoorLinks)
		PAUSE(("Number of DoorLinks Calculated does not match numDoorLinks"));

	return(numPackets);
}

void GlobalMap::calcSpecialAreas (MissionMapCellInfo* mapData) {

	bool areaLogged[64000];
	memset(areaLogged, false, 64000);
	long biggestID = -1;
	long smallestID = 99999;
	for (long r = 0; r < height; r++)
		for (long c = 0; c < width; c++) {
			if (mapData[r * width + c].specialType != SPECIAL_NONE) {
				long ID = mapData[r * width + c].specialID;
				if (ID > biggestID)
					biggestID = ID;
				if (ID < smallestID)
					smallestID = ID;
				switch (mapData[r * width + c].specialType) {
					case SPECIAL_GATE:
						if (!areaLogged[ID]) {
							specialAreas[ID].type = SPECIAL_GATE;
							specialAreas[ID].numSubAreas = 0;
							specialAreas[ID].numCells = 0;
							areaLogged[ID] = true;
						}
						areaMap[r * width + c] = ID;
						GameMap->setBuildGate(r, c, true);
						specialAreas[ID].cells[specialAreas[ID].numCells][0] = r;
						specialAreas[ID].cells[specialAreas[ID].numCells][1] = c;
						specialAreas[ID].numCells++;
						break;
					case SPECIAL_WALL:
						if (!areaLogged[ID]) {
							specialAreas[ID].type = SPECIAL_WALL;
							specialAreas[ID].numSubAreas = 0;
							specialAreas[ID].numCells = 0;
							areaLogged[ID] = true;
						}
						areaMap[r * width + c] = ID;
						GameMap->setBuildWall(r, c, true);
						specialAreas[ID].cells[specialAreas[ID].numCells][0] = r;
						specialAreas[ID].cells[specialAreas[ID].numCells][1] = c;
						specialAreas[ID].numCells++;
						break;
					case SPECIAL_LAND_BRIDGE:
						if (!areaLogged[ID]) {
							specialAreas[ID].type = SPECIAL_LAND_BRIDGE;
							specialAreas[ID].numSubAreas = 0;
							specialAreas[ID].numCells = 0;
							areaLogged[ID] = true;
						}
						areaMap[r * width + c] = ID;
						GameMap->setBuildLandBridge(r, c, true);
						specialAreas[ID].cells[specialAreas[ID].numCells][0] = r;
						specialAreas[ID].cells[specialAreas[ID].numCells][1] = c;
						specialAreas[ID].numCells++;
						break;
				}
			}
		}
	if (biggestID > -1)
		numSpecialAreas = (biggestID + 1);
	for (long i = 0; i < numSpecialAreas; i++) {
		specialAreas[i].numSubAreas = 0;
	}
}

long GlobalMap::setTempArea (long tileR, long tileC, long cost) {

	long numStartDoors = 0;
	for (long dir = 0; dir < 4; dir++) {
		long adjR = tileR + adjTile[dir][0];
		long adjC = tileC + adjTile[dir][1];
		long adjArea = calcArea(adjR, adjC);
		if (adjArea > -1) {
		}
	}
	areas[numAreas].numDoors = numStartDoors;

	areas[numAreas].sectorR = tileR / SECTOR_DIM;
	areas[numAreas].sectorC = tileC / SECTOR_DIM;
	return(numAreas);
}

bool GlobalMap::fillSpecialArea (long row, long col, long area, long specialID) {

	areaMap[row * width + col] = area;
	GameMap->setBuildNotSet(row, col, false);

	for (long dir = 0; dir < 4; dir ++) {
		long adjR = row + adjTile[dir][0];
		long adjC = col + adjTile[dir][1];
		if ((adjR >= minRow) && (adjR < maxRow) && (adjC >= minCol) && (adjC < maxCol))
			if (GameMap->getBuildNotSet(adjR, adjC) && (areaMap[adjR * width + adjC] == specialID))
				fillSpecialArea(adjR, adjC, area, specialID);
	}
	
	return(true);
}

bool GlobalMap::fillArea (long row, long col, long area, bool offMap) {

	if ((row < minRow) || (row >= maxRow) || (col < minCol) || (col >= maxCol))
		return(false);

	if (!blank)
		if (GameMap->getBuildSpecial(row, col))
			return(false);

	if (offMap) {
		if (!GameMap->getOffMap(row, col))
			return(false);
		}
	else {
		if (GameMap->getOffMap(row, col))
			return(false);
	}
	
	GameMap->setBuildNotSet(row, col, false);

	if (!blank) {
		if (!hover && GameMap->getDeepWater(row, col)) {
			areaMap[row * width + col] = -2;
			return(false);
		}

		if (!offMap && !GameMap->getPassable(row, col)) {
			areaMap[row * width + col] = -2;
			return(false);
		}
	}

	gosASSERT(area != -1);
	areaMap[row * width + col] = area;
	for (long dir = 0; dir < 4; dir ++) {
		long adjR = row + adjTile[dir][0];
		long adjC = col + adjTile[dir][1];
		if ((adjR >= minRow) && (adjR < maxRow) && (adjC >= minCol) && (adjC < maxCol))
			if (GameMap->getBuildNotSet(adjR, adjC))
				fillArea(adjR, adjC, area, offMap);
	}
	
	return(true);
}

void GlobalMap::calcSectorAreas (long sectorR, long sectorC) {

	minRow = sectorR * SECTOR_DIM;
	maxRow = minRow + SECTOR_DIM;
	minCol = sectorC * SECTOR_DIM;
	maxCol = minCol + SECTOR_DIM;

	if (blank) {
		for (long r = minRow; r < maxRow; r++)
			for (long c = minCol; c < maxCol; c++)
				if (GameMap->getBuildNotSet(r, c)) {
					if (fillArea(r, c, numAreas, GameMap->getOffMap(r, c)))
						numAreas++;
				}
		}
	else {
		for (long r = minRow; r < maxRow; r++)
			for (long c = minCol; c < maxCol; c++)
				if (GameMap->getBuildNotSet(r, c)) {
					if (GameMap->getBuildSpecial(r, c)) {
						long specialHere = areaMap[r * width + c];
						gosASSERT(specialHere > -1);
						if (specialAreas[specialHere].numSubAreas == MAX_SPECIAL_SUB_AREAS)
							Fatal(MAX_SPECIAL_SUB_AREAS, " GlobalMap.calcSectorAreas: too many special subareas ");
						specialAreas[specialHere].subAreas[specialAreas[specialHere].numSubAreas] = numAreas;
						specialAreas[specialHere].numSubAreas++;
						fillSpecialArea(r, c, numAreas++, specialHere);
						}
					else if (fillArea(r, c, numAreas, GameMap->getOffMap(r, c)))
						numAreas++;
				}
	}
}

void GlobalMap::calcAreas (void) {

	// Large area maps use -1 and -2 to indicate blocked area. CalcArea will
	// always return -1 for blocked-area cells.


	for (long r = 0; r < sectorHeight; r++)
		for (long c = 0; c < sectorWidth; c++)
			calcSectorAreas(r, c);

	// NOTE: We allocate one extra area--this is used by the calcPath routine
	// as a "scratch" area in some cases...
	gosASSERT(numAreas <= MAX_GLOBALMAP_AREAS);
	areas = (GlobalMapAreaPtr)systemHeap->Malloc(sizeof(GlobalMapArea) * (numAreas + 1));
	gosASSERT(areas != NULL);
	// Build path: areas_doors and areas_cellsCovered are not pre-allocated here
	// (the packet-load path in init(PacketFile, packet) allocates them before calling
	// calcAreas). Allocate them now so the loop below doesn't crash on NULL dereference.
	if (!areas_doors) {
		areas_doors = (DoorInfoPtr*)systemHeap->Malloc(sizeof(DoorInfoPtr) * (numAreas + 1));
		gosASSERT(areas_doors != NULL);
	}
	if (!areas_cellsCovered) {
		areas_cellsCovered = (short**)systemHeap->Malloc(sizeof(short*) * (numAreas + 1));
		gosASSERT(areas_cellsCovered != NULL);
	}
	for (long i = 0; i < (numAreas + 1); i++) {
		areas[i].sectorR = 0;
		areas[i].sectorC = 0;
		areas[i].type = AREA_TYPE_NORMAL;
		areas[i].numDoors = 0;
		areas[i].doors_Legacy32bitPtr = 0;
		areas_doors[i] = nullptr;
		areas[i].ownerWID = 0;
		areas[i].teamID = -1;
		areas[i].offMap = false;
		areas[i].open = true;
		areas[i].cellsCovered_Legacy32bitPtr = 0;
		areas_cellsCovered[i] = nullptr;
	}

	for (long sectorR = 0; sectorR < sectorHeight; sectorR++)
		for (long sectorC = 0; sectorC < sectorWidth; sectorC++) {
			minRow = sectorR * SECTOR_DIM;
			maxRow = minRow + SECTOR_DIM;
			minCol = sectorC * SECTOR_DIM;
			maxCol = minCol + SECTOR_DIM;
			for (long r = minRow; r < maxRow; r++)
				for (long c = minCol; c < maxCol; c++) {
					long curArea = areaMap[r * width + c];
					if (curArea > -1) {
						areas[curArea].sectorR = sectorR;
						areas[curArea].sectorC = sectorC;
					}
				}
		}
}

void GlobalMap::calcCellsCovered (void) {

}

void GlobalMap::calcSpecialTypes (void) {

	for (long i = 0; i < numSpecialAreas; i++) {
		if (specialAreas[i].type == SPECIAL_WALL)
			for (long j = 0; j < specialAreas[i].numSubAreas; j++)
				areas[specialAreas[i].subAreas[j]].type = AREA_TYPE_WALL;
		else if (specialAreas[i].type == SPECIAL_GATE)
			for (long j = 0; j < specialAreas[i].numSubAreas; j++)
				areas[specialAreas[i].subAreas[j]].type = AREA_TYPE_GATE;
		else if (specialAreas[i].type == SPECIAL_LAND_BRIDGE)
			for (long j = 0; j < specialAreas[i].numSubAreas; j++)
				areas[specialAreas[i].subAreas[j]].type = AREA_TYPE_LAND_BRIDGE;
		else if (specialAreas[i].type == SPECIAL_FOREST)
			for (long j = 0; j < specialAreas[i].numSubAreas; j++)
				areas[specialAreas[i].subAreas[j]].type = AREA_TYPE_FOREST;
	}

	for (long r = 0; r < height; r++)
		for (long c = 0; c < width; c++)
			if (GameMap->getOffMap(r, c)) {
				long area = calcArea(r, c);
				if (area > -1)
					areas[area].offMap = true;
			}
}

void GlobalMap::beginDoorProcessing (void) {

	doorBuildList = (GlobalMapDoorPtr)systemHeap->Malloc(sizeof(GlobalMapDoor) * MAX_GLOBALMAP_DOORS);
	for (long i = 0; i < MAX_GLOBALMAP_DOORS; i++) {
		doorBuildList[i].row = -1;
		doorBuildList[i].col = -1;
		doorBuildList[i].length = -1;
		doorBuildList[i].open = true;
		doorBuildList[i].teamID = -1;
		doorBuildList[i].numLinks[0] = 0;
		doorBuildList[i].numLinks[1] = 0;
	}
	gosASSERT(doorBuildList != NULL);
}

void GlobalMap::addDoor (long area1, long area2, long row, long col, long length, long dir) {


	bool newDoor = true;
	for (long curDoorIndex = 0; curDoorIndex < numDoors; curDoorIndex++) {
		GlobalMapDoorPtr curDoor = &doorBuildList[curDoorIndex];
		if ((curDoor->row == row) && (curDoor->col == col))
			if ((curDoor->length == length) && (curDoor->direction[0] == dir))
				newDoor = false;
	}

	if (newDoor) {
		doorBuildList[numDoors].row = row;
		doorBuildList[numDoors].col = col;
		doorBuildList[numDoors].length = length;
		doorBuildList[numDoors].open = true;
		doorBuildList[numDoors].teamID = -1;
		doorBuildList[numDoors].area[0] = area1;
		doorBuildList[numDoors].areaCost[0] = 1;
		doorBuildList[numDoors].direction[0] = dir;
		doorBuildList[numDoors].area[1] = area2;
		doorBuildList[numDoors].areaCost[1] = 1;
		doorBuildList[numDoors].direction[1] = (dir + 2) % 4;
		numDoors++;
		if (numDoors >= MAX_GLOBALMAP_DOORS)
			Fatal(numDoors, " Too many Global Doors ");
	}

	if (numDoors >= MAX_GLOBALMAP_DOORS)
		STOP(("Too Many Doors %d", numDoors));
}

void GlobalMap::endDoorProcessing (void) {

	if (doorBuildList) {
		// First, save the door list. Note that we make 2 extra doors, to be used
		// by the pathfinder...
		doors = (GlobalMapDoorPtr)systemHeap->Malloc(sizeof(GlobalMapDoor) * (numDoors + NUM_DOOR_OFFSETS));
		memcpy(doors, doorBuildList, sizeof(GlobalMapDoor) * (numDoors + NUM_DOOR_OFFSETS));
		systemHeap->Free(doorBuildList);
		doorBuildList = nullptr;
		// Build path: doors_links is only pre-allocated in the packet-load path
		// (init(PacketFile, packet) line ~1765).  Allocate it here so calcDoorLinks()
		// doesn't crash on a NULL dereference when indexing doors_links[d].
		if (!doors_links) {
			doors_links = (DoorInfoLinksPtr*)systemHeap->Malloc(sizeof(DoorInfoLinksPtr) * (numDoors + NUM_DOOR_OFFSETS));
			gosASSERT(doors_links != NULL);
			memset(doors_links, 0, sizeof(DoorInfoLinksPtr) * (numDoors + NUM_DOOR_OFFSETS));
		}
	}
}

long GlobalMap::numAreaDoors (long area) {

	long doorCount = 0;
	for (long doorIndex = 0; doorIndex < numDoors; doorIndex++)
		if ((doors[doorIndex].area[0] == area) || (doors[doorIndex].area[1] == area))
			doorCount++;
	if (doorCount > 255)
		Fatal(doorCount, " Too many area doors ");
	return(doorCount);
}

void GlobalMap::getAreaDoors (long area, DoorInfoPtr doorList) {

	long doorCount = 0;
	for (long doorIndex = 0; doorIndex < numDoors; doorIndex++)
		if ((doors[doorIndex].area[0] == area) || (doors[doorIndex].area[1] == area)) {
			doorList[doorCount].doorIndex = doorIndex;
			long doorSide = (doors[doorIndex].area[1] == area);
			doorList[doorCount].doorSide = doorSide;
			doorCount++;
		}
}
