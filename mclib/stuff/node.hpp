//===========================================================================//
// File:	node.hh                                                          //
// Contents: Interface specification for base node class                     //
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#pragma once

#include"stuff.hpp"
#include"socket.hpp"

namespace Stuff {

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ Node ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

	using Node__ClassData = Plug__ClassData;

	class Node:
		public Plug
	{
	public:
		static void
			InitializeClass();
		static void
			TerminateClass();

		void
			TestInstance();

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	// Constructor/Destructor
	//
	public:
		~Node();

	protected:
		explicit Node(ClassData *class_data);

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	// Class Data Support
	//
	public:
		using ClassData = Node__ClassData;

		static ClassData
			*DefaultData;

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	// plug detachment
	//
	public:
		virtual void
			ReleaseLinkHandler(Socket*, Plug*);
	};

}
