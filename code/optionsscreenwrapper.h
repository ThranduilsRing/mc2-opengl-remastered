#ifndef OPTIONSSCREENWRAPPER_H
#define OPTIONSSCREENWRAPPER_H
/*************************************************************************************************\
OptionsScreenWrapper.h			: Interface for the OptionsScreenWrapper component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

//*************************************************************************************************

/**************************************************************************************************
CLASS DESCRIPTION
OptionsScreenWrapper:
**************************************************************************************************/

#include"asystem.h"
#include"alistbox.h"
#include <memory>


class OptionsXScreen;

class OptionsScreenWrapper
{
	public:
		enum class status_type
		{
			opt_INACTIVE, 
			opt_ACTIVE, 
			opt_DONE
		};
		using enum status_type;

		OptionsScreenWrapper();
		~OptionsScreenWrapper();

		void init();
		void destroy();

		status_type update();
		void render();

		void begin();
		void end();
		bool isDone() { return (!isActive); }

	private:

		std::unique_ptr<OptionsXScreen>	pOptionsScreen;
		bool		isActive;

};





//*************************************************************************************************
#endif  // end of file ( OptionsScreenWrapper.h )
