//===========================================================================//
// File:	random.cc                                                        //
// Contents: Interface specification for the random number generator         //
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#pragma once

#include <cstdint>
#include <array>
#include <memory>
#include"stuff.hpp"
#include"scalar.hpp"

namespace Stuff {

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ Random ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

	class Random
		#if defined(_ARMOR)
			: public Stuff::Signature
		#endif
	{
	public:
		static std::uint64_t GetDrawCount();
		struct State
		{
			std::array<int, 250> numbers;
			int index;
			std::uint64_t draws;
			bool operator==(const State&) const = default;
		};
		static State GetState();

		static void
			InitializeClass();
		static void
			TerminateClass();

		// Per-mission restart of the effect table from the mission seed (deterministic, so effect-driven lifetimes replay).
		static void
			Reseed(unsigned int seed);

	private:
		static int
			Numbers[250];		// the random number table
		static int
			Index;					// the current entry within the table

		static void
			Init();

		static int
			GetRandomInt();

		Random()
			{Verify(Index == -1); Init();}

		static std::unique_ptr<Random>
			Instance;	// process singleton, owned between InitializeClass and TerminateClass
		std::uint64_t m_drawCount = 0;

		void
			TestInstance() const {}

	public:
		//
		//------------------------
		// Random number functions
		//------------------------
		//

        // sebi: that was RAND_MAX when MC2 was written, see routines.cpp
        static int GetRandMax() { return (1<<15)-1; }

		static int
			GetInt()						// returns 0 ..GetRandMax() 
				{return GetRandomInt();}
		static Scalar
			GetFraction();				// returns 0.0f <= x < 1.0f
		static int						// returns 0 .. Range-1
			GetLessThan(int Range);

		static bool
      	TestClass();
	};

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ Die ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

	class Die
		#if defined(_ARMOR)
			: public Stuff::Signature
		#endif
	{
	private:
		int
			highestRandom,	// the highest random number giving a uniform dist.
			dieSides;		// the number of sides on the die (starting from 1)

	public:
		Die(int sides);
		operator int();	// returns 1 .. dieSides
	};

}
