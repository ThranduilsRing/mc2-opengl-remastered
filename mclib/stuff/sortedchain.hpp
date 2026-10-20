//===========================================================================//
// File:	vchain.hh                                                        //
// Contents: Interface specification of SortedChain class                    //
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#pragma once

#include"stuff.hpp"
#include"node.hpp"
#include"sortedsocket.hpp"


namespace Stuff {

	class SortedChain;
	class SortedChainIterator;

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ SortedChainLink ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

	class SortedChainLink:
		public Link
	{
		friend class SortedChain;
		friend class SortedChainIterator;

	public:
		~SortedChainLink();
		void
			TestInstance();

	protected:
		SortedChainLink(
			SortedChain *vchain,
			Plug *plug
		);

	private:
		void
			SetupSortedChainLinks(
				SortedChainLink *next,
				SortedChainLink *prev
			);

		SortedChainLink *next;
		SortedChainLink *prev;
	};

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ SortedChainLinkOf ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

	inline constexpr int VChainLink_Memoryblock_Allocation = 100;

	template <class V> class SortedChainLinkOf:
		public SortedChainLink
	{
	public:
		SortedChainLinkOf(
			SortedChain *vchain,
			Plug *plug,
			const V &value
		);
		~SortedChainLinkOf();



		V
			GetValue()
				{return value;}
		V*
			GetValuePointer()
				{return &value;}

	private:


		V value;
	};

	//~~~~~~~~~~~~~~~~~~~~~~~~~~ SortedChainLinkOf templates ~~~~~~~~~~~~~~~~~~~~~~~~



	template <class V>
		SortedChainLinkOf<V>::SortedChainLinkOf(
			SortedChain *vchain,
			Plug *plug,
			const V &value
		):
			SortedChainLink(vchain, plug)
	{
		this->value = value;
	}

	template <class V>
		SortedChainLinkOf<V>::~SortedChainLinkOf()
	{
	}





	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ SortedChain ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

	class SortedChain:
		public SortedSocket
	{
		friend class SortedChainLink;
		friend class SortedChainIterator;

	public:
		//
		//--------------------------------------------------------------------
		//--------------------------------------------------------------------
		// Public interface
		//--------------------------------------------------------------------
		//--------------------------------------------------------------------
		//

		//
		//--------------------------------------------------------------------
		// Constructor, Destructor and testing
		//--------------------------------------------------------------------
		//
		SortedChain(
			Node *node,
			bool has_unique_entries
		);
		~SortedChain();

		void
			TestInstance();
		static bool
			TestClass();
		static bool
			ProfileClass();

		//
		//-----------------------------------------------------------------------
		// IsEmpty - Returns true if the socket contains no plugs.
		//-----------------------------------------------------------------------
		//
		bool
			IsEmpty();

	protected:
		//
		//--------------------------------------------------------------------
		//--------------------------------------------------------------------
		// Protected interface
		//--------------------------------------------------------------------
		//--------------------------------------------------------------------
		//
		void
			AddImplementation(Plug *);
		void
			AddValueImplementation(
				Plug *plug,
				const void *value
			);
		Plug
			*FindImplementation(const void *value);

	private:
		//
		//--------------------------------------------------------------------
		//--------------------------------------------------------------------
		// Private interface
		//--------------------------------------------------------------------
		//--------------------------------------------------------------------
		//
		virtual SortedChainLink
			*MakeSortedChainLink(
				Plug *plug,
				const void *value
			);
		virtual int
			CompareSortedChainLinks(
				SortedChainLink *link1,
				SortedChainLink *link2
			);
		virtual int
			CompareValueToSortedChainLink(
				const void *value,
				SortedChainLink *link
			);

		SortedChainLink*
			SearchForValue(const void *value);

		//
		//--------------------------------------------------------------------
		// Private data
		//--------------------------------------------------------------------
		//
		SortedChainLink *head;
      SortedChainLink *tail;
	};

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ SortedChainOf ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

	inline constexpr int VChain_Memoryblock_Allocation = 100;

	template <class T, class V> class SortedChainOf:
		public SortedChain
	{
	public:
		//
		//--------------------------------------------------------------------
		//--------------------------------------------------------------------
		// Public interface
		//--------------------------------------------------------------------
		//--------------------------------------------------------------------
		//
		SortedChainOf(
			Node *node,
			bool has_unique_entries
		);
		~SortedChainOf();



		//
		//--------------------------------------------------------------------
		// Socket methods (see Socket for full listing)
		//--------------------------------------------------------------------
		//
		void
			AddValue(
				T plug,
				const V &value
			)
				{AddValueImplementation(Cast_Object(Plug*,plug), &value);}
		void
			Remove(T plug)
				{RemovePlug(Cast_Object(Plug*,plug));}
		T
			Find(const V &value)
				{return (T)FindImplementation(&value);}

	private:
		//
		//--------------------------------------------------------------------
		//--------------------------------------------------------------------
		// Private interface
		//--------------------------------------------------------------------
		//--------------------------------------------------------------------
		//
		SortedChainLink
			*MakeSortedChainLink(
				Plug *plug,
				const void *value
			)
				{
					return
						new SortedChainLinkOf<V>(
							this,
							plug,
							*Cast_Pointer(const V*, value)
						);
				}
		int
			CompareSortedChainLinks(
				SortedChainLink *link1,
				SortedChainLink *link2
			);
		int
			CompareValueToSortedChainLink(
				const void *value,
				SortedChainLink *link
			);

		//
		//--------------------------------------------------------------------
		//--------------------------------------------------------------------
		// Private data
		//--------------------------------------------------------------------
		//--------------------------------------------------------------------
		//

	};

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~ SortedChainOf templates ~~~~~~~~~~~~~~~~~~~~~~~~~~~



	template <class T, class V>
		SortedChainOf<T, V>::SortedChainOf(
			Node *node,
			bool has_unique_entries
		):
			SortedChain(
				node,
				has_unique_entries
			)
	{
	}

	template <class T, class V>
		SortedChainOf<T, V>::~SortedChainOf()
	{
	}

	template <class T, class V> int
		SortedChainOf<T, V>::CompareSortedChainLinks(
			SortedChainLink *node1,
			SortedChainLink *node2
		)
	{
		V *ptr1 = Cast_Object(SortedChainLinkOf<V>*, node1)->GetValuePointer();
		V *ptr2 = Cast_Object(SortedChainLinkOf<V>*, node2)->GetValuePointer();

		Check_Pointer(ptr1);
		Check_Pointer(ptr2);

		if (*ptr1 == *ptr2)
			return 0;
		else
			return ((*ptr1 > *ptr2) ? 1 : -1);
	}

	template <class T, class V> int
		SortedChainOf<T, V>::CompareValueToSortedChainLink(
			const void *value,
			SortedChainLink *node
		)
	{
		Check_Pointer(value);

		V *ptr = Cast_Object(SortedChainLinkOf<V>*, node)->GetValuePointer();
		Check_Pointer(ptr);

		if (*Cast_Pointer(const V*, value) == *ptr)
			return 0;
		else
			return (*Cast_Pointer(const V*, value) > *ptr) ? 1 : -1;
	}





	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~ SortedChainIterator ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

	class SortedChainIterator:
		public SortedIterator
	{
	public:
		//
		//--------------------------------------------------------------------
		//--------------------------------------------------------------------
		// Public interface
		//--------------------------------------------------------------------
		//--------------------------------------------------------------------
		//

		//
		//--------------------------------------------------------------------
		// Constructors, Destructor and testing
		//--------------------------------------------------------------------
		//
		explicit SortedChainIterator(SortedChain *vchain);
		explicit SortedChainIterator(const SortedChainIterator *iterator);
		Iterator*
			MakeClone();
		~SortedChainIterator();
		void
			TestInstance() const;

		//
		//--------------------------------------------------------------------
		// Iterator methods (see Iterator for full listing)
		//--------------------------------------------------------------------
		//
		void
			First();
		void
			Last();
		void
			Next();
		void
			Previous();
		CollectionSize
			GetSize();
		void
			Remove();

	protected:
		//
		//--------------------------------------------------------------------
		//--------------------------------------------------------------------
		// Protected interface
		//--------------------------------------------------------------------
		//--------------------------------------------------------------------
		//
		void*
			ReadAndNextImplementation();
		void*
			ReadAndPreviousImplementation();
		void*
			GetCurrentImplementation();
		void*
			GetNthImplementation(
				CollectionSize index
			);
		Plug*
			FindImplementation(const void *value);

	protected:
		//
		//--------------------------------------------------------------------
		// Protected data
		//--------------------------------------------------------------------
		//
		SortedChainLink
			*currentLink;

	private:
		//
		//--------------------------------------------------------------------
		//--------------------------------------------------------------------
		// Private interface
		//--------------------------------------------------------------------
		//--------------------------------------------------------------------
		//
		void
			ReceiveMemo(
				IteratorMemo memo,
				void *content
			);
	};

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~ SortedChainIteratorOf ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

	template <class T, class V> class SortedChainIteratorOf:
		public SortedChainIterator
	{
	public:
		//
		//--------------------------------------------------------------------
		//--------------------------------------------------------------------
		// Public interface
		//--------------------------------------------------------------------
		//--------------------------------------------------------------------
		//

		//
		//--------------------------------------------------------------------
		// Constructors and Destructor
		//--------------------------------------------------------------------
		//
		explicit SortedChainIteratorOf(SortedChainOf<T, V> *vchain);
		explicit SortedChainIteratorOf(const SortedChainIteratorOf<T, V> &iterator);
		Iterator*
			MakeClone();
		~SortedChainIteratorOf();

		//
		//--------------------------------------------------------------------
		// Iterator methods (see Iterator for full listing)
		//--------------------------------------------------------------------
		//
		T
			ReadAndNext()
				{return (T)ReadAndNextImplementation();}
		T
			ReadAndPrevious()
				{return (T)ReadAndPreviousImplementation();}
		T
			GetCurrent()
				{return (T)GetCurrentImplementation();}
		T
			GetNth(CollectionSize index)
				{return (T)GetNthImplementation(index);}
		T
			Find(const V &value)
				{return (T)FindImplementation(&value);}
		V
			GetValue()
				{return Cast_Object(SortedChainLinkOf<V>*, currentLink)->GetValue();}
	};

	//~~~~~~~~~~~~~~~~~~~~~~~~ SortedChainIteratorOf templates ~~~~~~~~~~~~~~~~~~~~~~

	template <class T, class V>
		SortedChainIteratorOf<T, V>::SortedChainIteratorOf(SortedChainOf<T, V> *vchain):
			SortedChainIterator(vchain)
	{
	}

	template <class T, class V>
		SortedChainIteratorOf<T, V>::SortedChainIteratorOf(
			const SortedChainIteratorOf<T, V> &iterator
		):
			SortedChainIterator(&iterator)
	{
	}

	template <class T, class V> Iterator*
		SortedChainIteratorOf<T, V>::MakeClone()
	{
		return new SortedChainIteratorOf<T, V>(*this);
	}

	template <class T, class V>
		SortedChainIteratorOf<T, V>::~SortedChainIteratorOf()
	{
	}

}
