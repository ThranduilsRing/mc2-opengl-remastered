//---------------------------------------------------------------------------
//
// Packet.h -- File contains the header for the Packet File class
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef PACKET_H
#define PACKET_H
//---------------------------------------------------------------------------
// Include Files

#ifndef DPACKET_H
#include"dpacket.h"
#endif

#ifndef FILE_H
#include"file.h"
#endif

#ifndef LZ_H
#include"lz.h"
#endif

//---------------------------------------------------------------------------
// Macro Definitions

inline constexpr unsigned int PACKET_FILE_VERSION = 0xFEEDFACE;
inline constexpr unsigned int NEW_PACKET_FLAG = 0xFEEDFACE;

// 3 bits of type info

inline constexpr long STORAGE_TYPE_RAW = 0x00L;
inline constexpr long STORAGE_TYPE_FWF = 0x01L; // file within file
inline constexpr long STORAGE_TYPE_LZD = 0x02L; // LZ Compressed Packet
inline constexpr long STORAGE_TYPE_HF = 0x03L; // Huffman Compressed Packet
inline constexpr long STORAGE_TYPE_ZLIB = 0x04L; // zLib Compressed Packet
inline constexpr long STORAGE_TYPE_NUL = 0x07L; // NULL packet.

inline constexpr int TYPE_SHIFT = 29; // Bit position of masked type
#define DEFAULT_PACKET_TYPE			STORAGE_TYPE_RAW
inline constexpr int ANY_PACKET_TYPE = 0x07;
inline constexpr long OFFSET_MASK = ((1L << TYPE_SHIFT) - 1L);

#define GetPacketType(offset) 		((static_cast<unsigned int>(offset))>>TYPE_SHIFT)
#define GetPacketOffset(offset)		(offset&OFFSET_MASK)
#define SetPacketType(offset,type)	((offset)+(static_cast<int>((type))<<TYPE_SHIFT))

#define TABLE_ENTRY(p) ((2+p)<<2)	// ((1+p)*sizeof(long))

//---------------------------------------------------------------------------
// Structure and Class Definitions

//---------------------------------------------------------------------------
class PacketFile : public File
{
	//Data Members
	//-------------
	protected:
		int numPackets;
		int currentPacket;
		int packetSize;
		int packetBase;

		int packetType;		
		int packetUnpackedSize;

		// Packet offsets and storage types, one int per packet, kept in memory while the file is open for reading or being written start to finish.
		// Empty means each entry is read from and written to the file directly.
		std::vector<int> m_seekTable;

		bool usesCheckSum;

	//Member Functions
	//-----------------
	protected:
		void clear (void);
		void atClose (void);
		long afterOpen (void);

	public:

		PacketFile (void);
		~PacketFile (void);
		
		virtual long open (const char* fName, FileMode _mode = READ, long numChildren = 50, bool doNotLower = false);
		virtual long open (FilePtr _parent, unsigned long fileSize, long numChildren = 50);
		
		virtual long create (const char* fName);
		virtual long createWithCase(const char* fName ); // don't strlwr for me please!
		virtual void close (void);

		void forceUseCheckSum (void)
		{
			usesCheckSum = TRUE;
		}

		int readPacketOffset (int packet, int *lastType = 0);
		int readPacket (int packet, unsigned char *buffer);
		int readPackedPacket (int packet, unsigned char *buffer);

		int seekPacket (int packet);

		void operator ++ (void);
		void operator -- (void);
		
		int getNumPackets (void);
		int getCurrentPacket (void);
		int getPacketOffset(void);

		int getPacketSize (void)
		{
			return packetUnpackedSize;
		}

		int getPackedPacketSize (void);
		int getStorageType (void);

		virtual FileClass getFileClass (void)
		{
			return PACKETFILE;
		}
			
		int checkSumFile (void);

		//-------------------------------------------
		// Functions to Write Packet Files
		void reserve (unsigned int count, bool withCheckSum = FALSE);
		int writePacket (int packet, MemoryPtr buffer, unsigned int nbytes, unsigned char p_type = ANY_PACKET_TYPE);
		int insertPacket (int packet, MemoryPtr buffer, unsigned int nbytes, unsigned char p_type = ANY_PACKET_TYPE);
		int writePacket (int packet, MemoryPtr buffer);

        template<typename T>
		int writePacket (int packet, T basic_type, unsigned char p_type = ANY_PACKET_TYPE) {
		    return writePacket (packet, reinterpret_cast<unsigned char*>(&basic_type), sizeof(basic_type), p_type);
        }
};


//---------------------------------------------------------------------------
#endif

//---------------------------------------------------------------------------
//
// Edit Log
//
// $Log: packetfile.h $
// Revision 1.1  1995/08/09 17:38:22  fsavage
// Initial revision
//
//
//---------------------------------------------------------------------------

