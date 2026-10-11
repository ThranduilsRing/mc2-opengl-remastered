#define OPTIONSSCREENWRAPPER_CPP
/*************************************************************************************************\
OptionsScreenWrapper.cpp			: Implementation of the OptionsScreenWrapper component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include"optionsscreenwrapper.h"
#include"mclib.h"
#include"optionsarea.h"
#include "mc2fmt.h"


OptionsScreenWrapper::OptionsScreenWrapper()
{
	pOptionsScreen = nullptr;
	isActive = false;
}

OptionsScreenWrapper::~OptionsScreenWrapper()
{
	if (pOptionsScreen && isActive)
	{
		end();
	}
}

void OptionsScreenWrapper::begin()
{
	isActive = true;
	if (!pOptionsScreen)
	{
 		pOptionsScreen = new OptionsXScreen();
		char path[256];
		strcpy( path, artPath );
		strcat( path, "mcl_options.fit" );
		
		FitIniFile file;
		if ( NO_ERR != file.open( path ) )
		{
			char error[256];
			mc2::fmt::to_buf(error, "couldn't open file {}", mc2::fmt::safe(path));
			Assert( 0, 0, error );
			return;
		}
		pOptionsScreen->init( &file );
	}
}

void OptionsScreenWrapper::init()
{

}

void OptionsScreenWrapper::destroy()
{
}

void OptionsScreenWrapper::end()
{
	delete pOptionsScreen;
	pOptionsScreen = nullptr;
	isActive = false;
}

OptionsScreenWrapper::status_type OptionsScreenWrapper::update()
{
	if ( pOptionsScreen && isActive)

	{
		userInput->setMouseCursor( mState_NORMAL );
		pOptionsScreen->update();
		if ( pOptionsScreen->isDone() )
		{
			pOptionsScreen->updateOptions();
			isActive = 0;
			return opt_DONE;
		}
		else
		{
			return opt_ACTIVE;
		}
	}
	else
	{
		return opt_INACTIVE;
	}
}

void OptionsScreenWrapper::render()
{
	if ( pOptionsScreen && isActive )
		pOptionsScreen->render();
}

