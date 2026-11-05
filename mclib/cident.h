//---------------------------------------------------------------------------
//
// cident.h - This file contains the class declarations for misc file stuff
//
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef CIDENT_H
#define CIDENT_H
//---------------------------------------------------------------------------
// Include files

#ifndef DSTD_H
#include"dstd.h"
#endif

#ifndef DIDENT_H
#include"dident.h"
#endif

#include "mc2_czstring.h"
#include "memfunc.h"
#include <string.h>
#include"platform_str.h"

//---------------------------------------------------------------------------
// Externs

//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
//	class IDString
//---------------------------------------------------------------------------
class IDString
{
	protected:

		char id[ID_SIZE];					// 8 characters and a terminator...

	public:

		void init (void)
		{
			MemSet(id,0);
		}

		void init (mc2::czstring new_id)
		{
			strncpy(id,new_id,ID_SIZE-1);   // pads unused ID with 0s!
			id[ID_SIZE-1] = 0;
		}

		void init (const IDString & new_ids)
		{
			strncpy(id,new_ids.id,ID_SIZE-1);
			id[ID_SIZE-1] = 0;
		}

		IDString (void)
		{
			init();
		}

		IDString (mc2::czstring new_id)
		{
			init(new_id);
		}

		IDString (const IDString & new_ids)
		{
			init(new_ids);
		}

		inline operator char * (void)
		{
			return id;
		}

		bool operator == (mc2::czstring other_id)
		{
			if( other_id[0]!=id[0] )
				return FALSE;
			if( other_id[0]==0 )
				return TRUE;

			if( other_id[1]!=id[1] )
				return FALSE;
			if( other_id[1]==0 )
				return TRUE;

			if( other_id[2]!=id[2] )
				return FALSE;
			if( other_id[2]==0 )
				return TRUE;

			if( other_id[3]!=id[3] )
				return FALSE;
			if( other_id[3]==0 )
				return TRUE;

			if( other_id[4]!=id[4] )
				return FALSE;
			if( other_id[4]==0 )
				return TRUE;

			if( other_id[5]!=id[5] )
				return FALSE;
			if( other_id[5]==0 )
				return TRUE;

			if( other_id[6]!=id[6] )
				return FALSE;
			if( other_id[6]==0 )
				return TRUE;

			return (other_id[7]==id[7]);
//			return (strncmp(id,other_id,ID_SIZE-1) == 0);
		}

		bool operator == (const IDString &other_ids)
		{
			if( other_ids.id[0]!=id[0] )
				return FALSE;
			if( other_ids.id[0]==0 )
				return TRUE;

			if( other_ids.id[1]!=id[1] )
				return FALSE;
			if( other_ids.id[1]==0 )
				return TRUE;

			if( other_ids.id[2]!=id[2] )
				return FALSE;
			if( other_ids.id[2]==0 )
				return TRUE;

			if( other_ids.id[3]!=id[3] )
				return FALSE;
			if( other_ids.id[3]==0 )
				return TRUE;

			if( other_ids.id[4]!=id[4] )
				return FALSE;
			if( other_ids.id[4]==0 )
				return TRUE;

			if( other_ids.id[5]!=id[5] )
				return FALSE;
			if( other_ids.id[5]==0 )
				return TRUE;

			if( other_ids.id[6]!=id[6] )
				return FALSE;
			if( other_ids.id[6]==0 )
				return TRUE;

			return (other_ids.id[7]==id[7]);
//			return (strncmp(id,other_ids.id,ID_SIZE-1) == 0);
		}

		bool operator != (mc2::czstring other_id)
		{
			return !(*this == other_id);
		}

		bool operator != (const IDString &other_ids)
		{
			return !(*this == other_ids);
		}

		void operator = (mc2::czstring new_id)
		{
			init(new_id);
		}

		void operator = (const IDString & new_ids)
		{
			init(new_ids);
		}

		bool isNull (void)
		{
	      return (id[0] == 0);
		}

		void standardize (void)
		{
			S_strupr(id);
		}
};

//--------------------------------------------------------------------------
class FullPathFileName
{
	//Data items...
	//-------------

	private:

	protected:

		char *fullName;

	public:

	//Member functions...
	//-------------------

	private:

	protected:

	public:

		void init (void)
		{
			fullName = nullptr;
		}

		void init (mc2::czstring dir_path, mc2::czstring name, mc2::czstring ext, bool do_not_make_lower = false);

		FullPathFileName (void)
		{
			init();
		}

		FullPathFileName (mc2::czstring dir_path, mc2::czstring name, mc2::czstring ext, bool do_not_make_lower = false)
		{
			init();
			init(dir_path, name, ext, do_not_make_lower);
		}

		inline operator const char * (void) const 
		{
			return fullName;
		}

		void destroy (void);

		void changeExt (mc2::czstring from, mc2::czstring to);

		~FullPathFileName (void);
};

//---------------------------------------------------------------------------
#endif

//---------------------------------------------------------------------------
//
// Edit Log
//
// $Log$
//
//---------------------------------------------------------------------------
