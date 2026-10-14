//===========================================================================//
// File:	random.cc                                                        //
// Contents: Interface specification for the random number generator         //
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#pragma once

#include <cstdint>
#include <array>
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
		// A borrowed effects-session stream; the caller owns its reset/save lifecycle.
		class Stream
		{
		public:
			static constexpr std::uint32_t Id = 0x56465852u;
			static constexpr std::uint32_t Version = 1u;
			static constexpr auto Algorithm = "LCG32-NR-next15";
			struct State
			{
				std::uint32_t seed, value;
				std::uint64_t draws;
				bool operator==(const State&) const = default;
			};
			explicit Stream(std::uint32_t rootSeed = 0) { Reset(rootSeed); }
			void Reset(std::uint32_t rootSeed) { m_state = {rootSeed ^ Id, rootSeed ^ Id, 0}; }
			State GetState() const { return m_state; }
			void Restore(State state) { m_state = state; }
			std::uint64_t GetDrawCount() const { return m_state.draws; }
			Scalar GetFraction();
		private:
			State m_state;
		};

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

		static Random
			*Instance;
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
