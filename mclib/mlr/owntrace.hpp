//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#if !defined(OWNTARCE_HPP)
	#define OWNTRACE_HPP


	class OwnTrace
	{
	public:
		OwnTrace(const char *);

		void Start();
		void Stop();

		void Print();

	private:
		MString name;
		int on, count;
		double sum;

		double start, end;
	};

	inline OwnTrace::OwnTrace(const char *n)
	{
		name = n;

		sum = 0;
		on = 0;
		count = 0;
	}

	inline void OwnTrace::Start()
	{
	}

	inline void OwnTrace::Stop()
	{
	}

	inline void OwnTrace::Print()
	{
	}

#endif