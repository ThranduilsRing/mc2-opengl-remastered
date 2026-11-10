// Multi shape layer: TG_AnimateShape and _TG_Animation: binary load / save, ASE animation import and animation state.
// Declared in the class headers; shared names in msl_internal.h.
#include "mc2_czstring.h"
#include "msl_internal.h"
#include "mc2fmt.h"
#include "../GameOS/gameos/utils/mc2log.h"


//-------------------------------------------------------------------------------
// class TG_AnimateShape
//-------------------------------------------------------------------------------
void _TG_Animation::SaveBinaryCopy (File *binFile)
{
	if (S_stricmp(nodeId,"NONE") != 0)
	{
        // sebi: nodeId is not generally TG_NODE_ID bytes long
        char buf[TG_NODE_ID] = {0};
        strncpy(buf, nodeId, TG_NODE_ID);
		binFile->write(reinterpret_cast<MemoryPtr>(buf),TG_NODE_ID);
		binFile->writeInt(-1);			//ShapeIds ALWAYS start with -1.  We will scan on frame 1 please!
		binFile->writeInt(numFrames);
		binFile->writeFloat(frameRate);
		binFile->writeFloat(tickRate);
		
		if (quat)
			binFile->writeInt(sizeof(glm::quat) * numFrames);
		else
			binFile->writeInt(0);
			
		if (pos)
			binFile->writeInt(sizeof(glm::vec3) * numFrames);
		else
			binFile->writeInt(0);
		
		if (quat)
			binFile->write(reinterpret_cast<MemoryPtr>(quat),sizeof(glm::quat) * numFrames);
			
		if (pos)
			binFile->write(reinterpret_cast<MemoryPtr>(pos),sizeof(glm::vec3) * numFrames);
	}
}

//-------------------------------------------------------------------------------
bool _TG_Animation::LoadBinaryCopy (File *binFile)
{
	binFile->read(reinterpret_cast<MemoryPtr>(nodeId),TG_NODE_ID);
	shapeId = binFile->readInt();
	numFrames = binFile->readInt();
	frameRate = binFile->readFloat();
	tickRate = binFile->readFloat();
	
	int quatRAM = binFile->readInt();
	int posRAM = binFile->readInt();		

	// Frame counts and track sizes are file content: a track must be exactly numFrames records, and no animation has this many frames.
	constexpr DWORD kMaxFrames = 100000;
	const bool sizesOK = numFrames <= kMaxFrames
		&& (quatRAM == 0 || static_cast<size_t>(quatRAM) == sizeof(glm::quat) * numFrames)
		&& (posRAM == 0 || static_cast<size_t>(posRAM) == sizeof(glm::vec3) * numFrames)
		&& quatRAM >= 0 && posRAM >= 0;
	if (!sizesOK)
	{
		MC2_LOG(Io, Error, "[ANIM_TRACK_BAD] node={} frames={} quatBytes={} posBytes={} (the sizes do not match the frame count; animation file rejected)",
			nodeId, numFrames, quatRAM, posRAM);
		numFrames = 0;
		quat = nullptr;
		pos = nullptr;
		return false;
	}

	if (quatRAM)
	{
		quatStore.resize(numFrames);
		quat = quatStore.data();
		binFile->read(reinterpret_cast<MemoryPtr>(quat),quatRAM);
	}
	else
		quat = nullptr;
		
	if (posRAM)
	{
		posStore.resize(numFrames);
		pos = posStore.data();
		binFile->read(reinterpret_cast<MemoryPtr>(pos),posRAM);
	}
	else
		pos = nullptr;
	return true;
}

//-------------------------------------------------------------------------------
long TG_AnimateShape::LoadBinaryCopy (mc2::czstring fileName)
{
	bindingId_ = nextAnimationBindingId();
	File binFile;
	long result = binFile.open(fileName);
	if (result != NO_ERR)
		return -1;
		
	int animFileVersion = binFile.readInt();
	if (animFileVersion == CURRENT_ANIM_VERSION)
	{
		count = binFile.readInt();
		
		if (count)
		{
			if (count < 0 || count > 4096)
			{
				MC2_LOG(Io, Error, "[ANIM_TRACK_BAD] file={} animations={} (not a plausible animation count; rejected)", fileName, count);
				count = 0;
				return -1;
			}
			animationStore = std::vector<TG_Animation>(static_cast<size_t>(count));
			listOfAnimation = animationStore.data();
 			
			for (long i=0;i<count;i++)
			{
				if (!listOfAnimation[i].LoadBinaryCopy(&binFile))
				{
					destroy();
					return -1;
				}
			}
		}
		else
			listOfAnimation = nullptr;
	}
	else
	{
		return -1;
	}
	
	return 0;
}

//-------------------------------------------------------------------------------
void TG_AnimateShape::SaveBinaryCopy (mc2::czstring fileName)
{
	File binFile;
	binFile.create(fileName);
	
	binFile.writeInt(CURRENT_ANIM_VERSION);
	
	//ListOfAnimations
	actualCount = 0;
	if (count)
	{
 		for (long i=0;i<count;i++)
		{
			if (S_stricmp(listOfAnimation[i].nodeId,"NONE") != 0)
				actualCount++;
		}
	}

	binFile.writeInt(actualCount);

	//ListOfAnimations
	if (count)
	{
 		for (long i=0;i<count;i++)
			listOfAnimation[i].SaveBinaryCopy(&binFile);
	}
}

//-------------------------------------------------------------------------------
//This function frees all of the RAM allocated by this class and resets vars to initial state.
void TG_AnimateShape::destroy (void)
{
	bindingId_ = nextAnimationBindingId();
	std::vector<TG_Animation>().swap(animationStore);
	listOfAnimation = nullptr;
	count = actualCount = 0;
}

FilePtr tglLogFile = static_cast<FilePtr>(nullptr);

//-------------------------------------------------------------------------------
//This function loads the animation data contained in the file passed in.
//It sets up a pointer to the multi-shape so that animation data for each
//Node in the Multi-Shape can be loaded.
//It mallocs memory.
long TG_AnimateShape::LoadTGMultiShapeAnimationFromASE (mc2::czstring fileName, TG_TypeMultiShapePtr shape, bool skipIfBinary, bool forceMakeBinary)
{
	bindingId_ = nextAnimationBindingId();
	//------------------------------------------------------
	// New!  Checks for Binary file to load instead.
	//
	// If Binary is older or equal to ASE, re-parses ASE and
	// saves binary.
	//
	// If Binary does not exist, does the above, too!
	// 
	// If Binary exists and is newer, just loads that.
	// MUCH FASTER!
	//
	bool makeBinary = false;

	char drive[MAX_PATH];
	char dir[MAX_PATH];
	char name[MAX_PATH];
	char ext[MAX_PATH];
	_splitpath(fileName,drive,dir,name,ext);

	FullPathFileName binaryFileName;
	binaryFileName.init(tglPath,name,".agl");

	if( !forceMakeBinary )
	{
		if (fileExists(binaryFileName) && !fileExists(fileName))
		{
			File binFile;
			
			long result = binFile.open(binaryFileName);
			if (result != NO_ERR)
			{
				makeBinary = true;
			}
			else
			{
				int versionId = binFile.readInt();
				if (versionId != CURRENT_ANIM_VERSION)
					makeBinary = true;
			}
		}
		else
		{
			makeBinary = true;
		}
	}
	else
	{
		makeBinary = true;
	}

	if (!makeBinary)
	{
		if (!skipIfBinary)
			return (LoadBinaryCopy(binaryFileName));
		else
			return 0;
	}
	else
	{
		//-----------------------------------------------------
		// Fully loads and parses an ASE File.  These files are
		// output by 3D Studio MAX's ASCII Scene Exporter.
		//
		shapeIdsSet = false;
	
		//------------------------------------------
		// Check if exists 
		if (!gos_FileExists(fileName))
			return(-1);
		
		long aseFileSize = gos_FileSize(fileName);
	
		//---------------------------------------
		// Create Buffer to read entire file into
		BYTE *aseContents = static_cast<BYTE *>(malloc(aseFileSize+1));
		gosASSERT(aseContents != nullptr);
	
		//---------------------------------------
		// Open File and read it and close it
		HGOSFILE aseFile;
		gos_OpenFile(&aseFile,fileName,READONLY);
	
	#ifdef _DEBUG
		long dataRead = 
	#endif
		gos_ReadFile(aseFile,aseContents,aseFileSize);
		gosASSERT(dataRead == aseFileSize);
	
		gos_CloseFile(aseFile);
	
		aseContents[aseFileSize] = 0;
	
		//----------------------------------------
		// Check for valid ASE Header data.
		if (S_strnicmp(ASE_HEADER,reinterpret_cast<char *>(aseContents),strlen(ASE_HEADER)) != 0)
			return(-1);
	
		//------------------------------------------
		// Get first frame of animation from header
		long firstFrame, lastFrame;
		char *frameId = strstr(reinterpret_cast<char *>(aseContents),ASE_ANIM_FIRST_FRAME);
		gosASSERT(frameId != nullptr);
		frameId += strlen(ASE_ANIM_FIRST_FRAME)+1;
	
		char numData[512];
		GetNumberData(frameId,numData);
		firstFrame = atol(numData);
	
		frameId = strstr(reinterpret_cast<char *>(aseContents),ASE_ANIM_LAST_FRAME);
		gosASSERT(frameId != nullptr);
	
		frameId += strlen(ASE_ANIM_LAST_FRAME)+1;
	
		GetNumberData(frameId,numData);
		lastFrame = atol(numData);
	
		//gosASSERT(firstFrame == 0);
		gosASSERT(firstFrame <= lastFrame);
	
		//if (firstFrame == lastFrame)
			//No Animation data at all. Possible?
	
		long numFrames = (lastFrame - firstFrame) + 1;
	
		frameId = strstr(reinterpret_cast<char *>(aseContents),ASE_ANIM_FRAME_SPEED);
		gosASSERT(frameId != nullptr);
	
		frameId += strlen(ASE_ANIM_FRAME_SPEED)+1;
	
		GetNumberData(frameId,numData);
		float frameRate = static_cast<float>(atof(numData));
	
		frameId = strstr(reinterpret_cast<char *>(aseContents),ASE_ANIM_TICKS_FRAME);
		gosASSERT(frameId != nullptr);
	
		frameId += strlen(ASE_ANIM_TICKS_FRAME)+1;
	
		GetNumberData(frameId,numData);
		float tickRate = static_cast<float>(atof(numData));
	
		//----------------------------------------------------------------------------
		// For each TG_Shape in MultiShape passed in, find any and all animation Data
		count = shape->GetNumShapes();
		animationStore = std::vector<TG_Animation>(static_cast<size_t>(count));
		listOfAnimation = animationStore.data();
	
		//----------------------------------------------------------------------------
		// Scan the Nodes.
		actualCount = 0;
		bool countUp = false;
		for (long i=0;i<count;i++)
		{
			char *nodeId = shape->GetNodeId(i);
			gosASSERT(nodeId != nullptr);
		
			char *animScan = reinterpret_cast<char *>(aseContents);
			animScan = strstr(animScan,ASE_ANIMATION);
	
			char nodeName[512];
			mc2::fmt::to_buf(nodeName, "*NODE_NAME \"{}\"", mc2::fmt::safe(nodeId));
			while (animScan != nullptr)
			{
				animScan += strlen(ASE_ANIMATION)+1;
	
				//------------------------------------------------------
				// We found a TM_ANIMATION Section.
				// Check if the VERY NEXT LINE is the correct NodeName
				char nextLine[1024];
				GetNextLine(animScan,nextLine);
	
				if (strstr(nextLine,nodeName) == nullptr)
				{
					animScan = strstr(animScan,ASE_ANIMATION);
				}
				else
				{
					animScan += strlen(nodeName)+1;
					break;
				}
			}
	
			if (animScan == nullptr)
			{
				//No Animation Data for this Node.
				strcpy(listOfAnimation[i].nodeId,"NONE");
				listOfAnimation[i].numFrames = numFrames;
				listOfAnimation[i].frameRate = frameRate;
				listOfAnimation[i].tickRate = tickRate;
				listOfAnimation[i].quat = nullptr;
				listOfAnimation[i].pos = nullptr;
				listOfAnimation[i].shapeId = 0xffffffff;
			}
			else
			{
				//Start with No Animation Data for this Node.
				strcpy(listOfAnimation[i].nodeId,"NONE");
				listOfAnimation[i].numFrames = numFrames;
				listOfAnimation[i].frameRate = frameRate;
				listOfAnimation[i].tickRate = tickRate;
				listOfAnimation[i].quat = nullptr;
				listOfAnimation[i].pos = nullptr;
				listOfAnimation[i].shapeId = 0xffffffff;

				//---------------------------------
				// Check for positional data first!
				char* scanStart = animScan;
	
				char numData[512];
				char nextLine[1024];
				float timeStamp = firstFrame * tickRate;
	
				//----------------------------------------------------
				// Then the very NEXT LINE most be POS_TRACK data OR
				// there is ONLY rotational Data for this node.
				GetNextLine(animScan,nextLine);
				if (strstr(nextLine,ASE_ANIM_POS_HEADER) != nullptr)
				{
					animScan = strstr(animScan,ASE_ANIM_POS_HEADER);
					if (animScan)
					{
						countUp = true;
						actualCount++;
						animScan += strlen(ASE_ANIM_POS_HEADER);
		
						//-----------------------------------------------------------
						// We have positional data at least.  Store everything off.
						listOfAnimation[i].posStore.resize(numFrames);
						listOfAnimation[i].pos = listOfAnimation[i].posStore.data();
		
						strcpy(listOfAnimation[i].nodeId,nodeId);
						listOfAnimation[i].numFrames = numFrames;
						listOfAnimation[i].frameRate = frameRate;
						listOfAnimation[i].tickRate = tickRate;
						listOfAnimation[i].shapeId = 0xffffffff;
		
						glm::vec3 thisOffset = shape->GetNodeCenter(i);
		
						char LineData[1024];
						GetNextLine(animScan,LineData);
						animScan += strlen(LineData)+1;
		
						for (long j=0;j<listOfAnimation[i].numFrames;j++)
						{
							char *scanData;
							mc2::fmt::to_buf(nodeName, "{} {:d}", mc2::fmt::safe(ASE_ANIM_POS_SAMPLE), mc2::fmt::as_i32(static_cast<int>(timeStamp)));
							scanData = strstr(LineData,nodeName);
		
							if (scanData)
							{
								scanData += strlen(nodeName)+1;
		
								GetNumberData(scanData,numData);
								thisOffset.x = -static_cast<float>(atof(numData));
								scanData += strlen(numData)+1;
		
								GetNumberData(scanData,numData);
								thisOffset.z = static_cast<float>(atof(numData));
								scanData += strlen(numData)+1;
		
								GetNumberData(scanData,numData);
								thisOffset.y = static_cast<float>(atof(numData));
								scanData += strlen(numData)+1;
		
								GetNextLine(animScan,LineData);
								animScan += strlen(LineData)+1;
							}
							else
							{
								//Otherwise Node Center did not move.  Use last Node Center
							}
		
							listOfAnimation[i].pos[j] = thisOffset;
							timeStamp += tickRate;
						}
					}
				}
											   
				//-------------------------------------------------------------
				// Check for rotational data. Again, use nextLine.
				
				//----------------------------------------------------
				// Then the very NEXT LINE most be POS_TRACK data OR
				// there is ONLY rotational Data for this node.
				GetNextLine(animScan,nextLine);
				if (strstr(nextLine,ASE_ANIM_ROT_HEADER) != nullptr)
				{
					animScan = scanStart;
					timeStamp = firstFrame * tickRate;
		
					animScan = strstr(animScan,ASE_ANIM_ROT_HEADER);
		
					if (animScan)
					{
						countUp = true;
						actualCount++;
						animScan += strlen(ASE_ANIM_ROT_HEADER);
		
						//-----------------------------------------------------------
						// We have rotational data at least.  Store everything off.
						listOfAnimation[i].quatStore.resize(numFrames);
						listOfAnimation[i].quat = listOfAnimation[i].quatStore.data();
		
						//-------------------------------------------
						// Setup basic variables.  May do this twice.
						strcpy(listOfAnimation[i].nodeId,nodeId);
						listOfAnimation[i].numFrames = numFrames;
						listOfAnimation[i].frameRate = frameRate;
						listOfAnimation[i].tickRate = tickRate;
		
						char LineData[1024];
						GetNextLine(animScan,LineData);
						animScan += strlen(LineData)+1;
			
						for (long j=0;j<listOfAnimation[i].numFrames;j++)
						{
							char *scanData;
							mc2::fmt::to_buf(nodeName, "{} {:d}", mc2::fmt::safe(ASE_ANIM_ROT_SAMPLE), mc2::fmt::as_i32(static_cast<int>(timeStamp)));
							scanData = strstr(LineData,nodeName);
		
							glm::quat thisFrame;
	
							float b=0.0f,c=0.0f,d=0.0f,phi=0.0f;
		
							if (scanData)
							{
								scanData += strlen(nodeName)+1;
		
								GetNumberData(scanData,numData);
								b = static_cast<float>(atof(numData));
								scanData += strlen(numData)+1;
		
								GetNumberData(scanData,numData);
								c = static_cast<float>(atof(numData));
								scanData += strlen(numData)+1;
		
								GetNumberData(scanData,numData);
								d = static_cast<float>(atof(numData));
								scanData += strlen(numData)+1;
		
								GetNumberData(scanData,numData);
								phi = static_cast<float>(atof(numData));
		
								GetNextLine(animScan,LineData);
								animScan += strlen(LineData)+1;
			
								//--------------------------------------------
								// MAX Writes out Quaternions as Angle, Axis.
								// Must Convert to real quaternion here.
								thisFrame.w = cos(phi / 2.0f);
								thisFrame.x = b * sin(phi / 2.0f);
								thisFrame.y = d * sin(-phi / 2.0f);
								thisFrame.z = c * sin(-phi / 2.0f);
							}
							else
							{
								//Otherwise rotation is 0.
								thisFrame.w = 1.0f;
								thisFrame.x = 0.0f;
								thisFrame.y = 0.0f;
								thisFrame.z = 0.0f;
							}
		
							if (!j)
							{
								thisFrame = glm::normalize(thisFrame);
								listOfAnimation[i].quat[j] = thisFrame;
							}
							else
							{
								thisFrame = glm::normalize(thisFrame);
								listOfAnimation[i].quat[j] = mc2::math::composeQuat(listOfAnimation[i].quat[j-1], thisFrame);
								listOfAnimation[i].quat[j] = glm::normalize(listOfAnimation[i].quat[j]);
							}
		
							timeStamp += tickRate;
						}
					}
				}
				
				countUp = false;
			}
		}
	
		free(aseContents);
		aseContents = nullptr;
		
		SaveBinaryCopy(binaryFileName);
	}

	return(0);
}	

//-------------------------------------------------------------------------------
//This function copies the pointers to the animation data in this class to the
//TG_MultiShape being drawn.  Nothing else happens.
const char* TG_AnimateShape::MissingAnimationNode(TG_TypeMultiShape* shape) const
{
	for (long j = 0; j < count; ++j)
	{
		if (!listOfAnimation[j].quat && !listOfAnimation[j].pos) continue;
		bool found = false;
		for (long i = 0; i < shape->GetNumShapes(); ++i)
			if (S_stricmp(listOfAnimation[j].nodeId, shape->GetNodeId(i)) == 0) { found = true; break; }
		if (!found) return listOfAnimation[j].nodeId;
	}
	return nullptr;
}

void TG_AnimateShape::SetAnimationState (TG_MultiShapePtr shape)
{
	const auto& indices = shape->animationBinding_.bind(bindingId_, count, shape->GetNumShapes(),
		[&](long track, long node) { return S_stricmp(listOfAnimation[track].nodeId, shape->GetNodeId(node)) == 0; });
	// Clear pointers without invalidating the map. Public ClearAnimation invalidates.
	for (long i = 0; i < shape->GetNumShapes(); ++i)
		shape->SetCurrentAnimation(i, nullptr);
	for (long j = 0; j < count; ++j)
		shape->SetCurrentAnimation(indices[j], &listOfAnimation[j]);
}	


void TG_MultiShape::Touch() {
	for (long i = 0; i < numTG_Shapes; i++)
		if (listOfShapes[i].node)
			listOfShapes[i].node->Touch();
}
