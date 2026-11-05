//===========================================================================//
// File:     notation.hpp                                                    //
// Title:    Declaration of NotationFile classes.                            //
// Purpose:  Provide general purpose access to data stored in a formatted    //
//           text file.                                                      //
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#pragma once

#include"stuff.hpp"
#include"notationfile.hpp"

namespace Stuff {

	//##########################################################################
	//##############    Page    ##############################
	//##########################################################################

	class Page:
		public Plug
	{
		friend class NotationFile;
		friend class Note;

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	// Constructor/Destructors
	//
	protected:
		Page(NotationFile *notation_file);
		~Page();

		NotationFile
			*m_notationFile;

	public:
		NotationFile*
			GetNotationFile()
				{Check_Object(this); return m_notationFile;}

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	// Page functions
	//
	public:
		void
			SetName(mc2::czstring pagename)
				{Check_Object(this); m_name = pagename;}
		const char*
			GetName() const
				{ Check_Object(this); return m_name; }

		void
			WriteNotes(MemoryStream *stream);

	protected:
		MString
			m_name;

		void
			SetDirty()
				{Check_Object(this); Check_Object(m_notationFile); m_notationFile->SetDirty();}

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	// Entry access
	//
	public:
		bool
			IsEmpty()
				{Check_Object(this); return m_notes.IsEmpty();}

		bool
			DoesNoteExist(mc2::czstring entryname)
				{Check_Object(this); return FindNote(entryname) != nullptr;}
		Note*
			FindNote(mc2::czstring entryname);
		Note*
			GetNote(unsigned index);

		using NoteIterator = ChainIteratorOf<Note* >;
		NoteIterator*
			MakeNoteIterator()
				{Check_Object(this); return new NoteIterator(&m_notes);}

		ChainOf<Note*>*
			MakeNoteChain(mc2::czstring prefix);

		Note*
			AddNote(mc2::czstring entryname);
		Note*
			SetNote(mc2::czstring entryname);

		void
			DeleteNote(mc2::czstring entryname);
		void
			DeleteAllNotes();

	protected:
		ChainOf<Note*>
			m_notes;

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	// string access
	//
	public:
		bool
			GetEntry(
				mc2::czstring entryname,
				const char **contents,
				bool required=false
			);
		void
			SetEntry(
				mc2::czstring entryname,
				const char *contents
			);
		void
			AppendEntry(
				mc2::czstring entryname,
				const char *contents
			);

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	// int access
	//
	public:
		bool
			GetEntry(
				mc2::czstring entryname,
				int *value,
				bool required=false
			);
		void
			SetEntry(
				mc2::czstring entryname,
				int value
			);
		void
			AppendEntry(
				mc2::czstring entryname,
				int value
			);

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	// scalar access
	//
	public:
		bool
			GetEntry(
				mc2::czstring entryname,
				Scalar *value,
				bool required=false
			);
		void
			SetEntry(
				mc2::czstring entryname,
				Scalar value
			);
		void
			AppendEntry(
				mc2::czstring entryname,
				Scalar value
			);

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	// bool access
	//
	public:
		bool
			GetEntry(
				mc2::czstring entryname,
				bool *value,
				bool required=false
			);
		void
			SetEntry(
				mc2::czstring entryname,
				bool value
			);
		void
			AppendEntry(
				mc2::czstring entryname,
				bool value
			);

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	// glm::vec3 access
	//
	public:
		bool
			GetEntry(
				mc2::czstring entryname,
				glm::vec3 *value,
				bool required=false
			);
		void
			SetEntry(
				mc2::czstring entryname,
				const glm::vec3 &value
			);
		void
			AppendEntry(
				mc2::czstring entryname,
				const glm::vec3 &value
			);

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	// RGBColor access
	//
	public:
		bool
			GetEntry(
				mc2::czstring entryname,
				RGBColor *value,
				bool required=false
			);
		void
			SetEntry(
				mc2::czstring entryname,
				const RGBColor &value
			);
		void
			AppendEntry(
				mc2::czstring entryname,
				const RGBColor &value
			);

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	// RGBAColor access
	//
	public:
		bool
			GetEntry(
				mc2::czstring entryname,
				RGBAColor *value,
				bool required=false
			);
		void
			SetEntry(
				mc2::czstring entryname,
				const RGBAColor &value
			);
		void
			AppendEntry(
				mc2::czstring entryname,
				const RGBAColor &value
			);

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	// NotationFile access
	//
	public:
		bool
			GetEntry(
				mc2::czstring entryname,
				NotationFile *value,
				bool required=false
			);
		void
			SetEntry(
				mc2::czstring entryname,
				NotationFile *value
			);
		void
			AppendEntry(
				mc2::czstring entryname,
				NotationFile *value
			);

	public:
		void
			TestInstance() const;
	};

}
