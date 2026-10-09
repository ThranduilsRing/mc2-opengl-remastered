//===========================================================================//
// File:     namelist.tst                                                    //
// Title:    Definition of NameList TestClass methods.                       //
// Purpose:  Maintains an unsorted list of strings with (void *) to          //
//           anything the client needs to associate with the string.         //
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include"stuffheaders.hpp"
#include"namelist.hpp"

#define TEST_COUNT 50

//#############################################################################
//##############    TestHost    ###############################################
//#############################################################################

class TestHost
	#if defined(_ARMOR)
		: public Stuff::Signature
	#endif
{
public:
	//--------------------------------------------------------------------
	// ObjectNameList is designed to bond with another class to provide
	// dymanic naming of the objects represented by that class.  TestHost
	// provides a means of testing the functionality of ObjectNameList.
	//--------------------------------------------------------------------
	const char
		*name;
	TestHost
		*next;
	static ObjectNameList
		names;

	TestHost(const char *a_name);
	~TestHost();

	bool
		TestInstance() const;
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
ObjectNameList
	TestHost::names;

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
TestHost::TestHost(const char *a_name)
{
	Check_Pointer(this);
	Check_Pointer(a_name);
	name = names.AddEntry(a_name, this);
	next = NULL;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
TestHost::~TestHost()
{
	Check_Object(this);
	names.DeleteEntry(name);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
bool
	TestHost::TestInstance() const
{
	return true;
}

//#############################################################################
//##############    ObjectNameList::TestClass    ##############################
//#############################################################################

bool
	ObjectNameList::TestClass()
{
	SPEW((GROUP_STUFF_TEST, "Starting ObjectNameList test..."));

	SPEW((GROUP_STUFF_TEST, "ObjectNameList test is stubbed out..."));

//	Tell("  ObjectNameList::TestClass() is stubbed out!\n");
	return true;
}

//#############################################################################
//##############    NameList::TestClass    ####################################
//#############################################################################

bool
	NameList::TestClass()
{
	SPEW((GROUP_STUFF_TEST, "Starting NameList test..."));

	SPEW((GROUP_STUFF_TEST, "  NameList::TestClass() is stubbed out!"));
	return false;
}

//#############################################################################
//##############    AlphaNameList::TestClass    ###############################
//#############################################################################

bool
	AlphaNameList::TestClass()
{
	SPEW((GROUP_STUFF_TEST, "Starting AlphaNameList test..."));

	SPEW((GROUP_STUFF_TEST, "  AlphaNameList::TestClass() is stubbed out!\n"));
	return false;
}

//#############################################################################
//#############################################################################
