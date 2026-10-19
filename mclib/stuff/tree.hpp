//===========================================================================//
// File:	tree.hh                                                          //
// Contents: Interface specification of Tree class                           //
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#pragma once

#include"stuff.hpp"
#include"node.hpp"
#include"sortedsocket.hpp"


namespace Stuff {

	class Tree;
	class TreeIterator;

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ TreeNode ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

	class TreeNode:
		public Link
	{
		friend class Tree;
		friend class TreeIterator;

	public:
		~TreeNode();
		void
			TestInstance();

	protected:
		TreeNode(
			Tree *tree,
			Plug *plug
		);

	private:
		void
			SetupTreeLinks(
				TreeNode *less,
				TreeNode *greater,
				TreeNode *parent
			);

		TreeNode *less;
		TreeNode *greater;
		TreeNode *parent;
	};

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ TreeNodeOf ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

	inline constexpr int TreeNode_MemoryBlock_Allocation = 100;

	template <class V> class TreeNodeOf:
		public TreeNode
	{
	public:
		TreeNodeOf(
			Tree *tree,
			Plug *plug,
			const V &value
		);
		~TreeNodeOf();



		V
			GetValue()
				{return value;}
		V*
			GetValuePointer()
				{return &value;}

	private:


		V value;
	};

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~ TreeNodeOf templates ~~~~~~~~~~~~~~~~~~~~~~~~~



	template <class V>
		TreeNodeOf<V>::TreeNodeOf(
			Tree *tree,
			Plug *plug,
			const V &value
		):
			TreeNode(tree, plug)
	{
		this->value = value;
	}

	template <class V>
		TreeNodeOf<V>::~TreeNodeOf()
	{
	}





	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ Tree ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

	class Tree:
		public SortedSocket
	{
		friend class TreeNode;
		friend class TreeIterator;

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
		Tree(
			Node *node,
			bool has_unique_entries
		);
		~Tree();

		void
			TestInstance();
		static void
			TestClass();
		static void
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
		virtual TreeNode*
      	MakeTreeNode(
				Plug *plug,
				const void *value
			);
		virtual int
			CompareTreeNodes(
				TreeNode *link1,
				TreeNode *link2
			);
		virtual int
			CompareValueToTreeNode(
				const void *value,
				TreeNode *link
			);

		void
			AddTreeNode(TreeNode *node);
		void
			SeverFromTreeNode(TreeNode *node);
		TreeNode*
			SearchForValue(const void *value);

		//
		//--------------------------------------------------------------------
		// Private data
		//--------------------------------------------------------------------
		//
		TreeNode *root;
	};

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ TreeOf ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

	template <class T, class V> class TreeOf:
		public Tree
	{
	public:
		//
		//--------------------------------------------------------------------
		//--------------------------------------------------------------------
		// Public interface
		//--------------------------------------------------------------------
		//--------------------------------------------------------------------
		//
		TreeOf(
			Node *node,
			bool has_unique_entries
		);
		~TreeOf();

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
		TreeNode
			*MakeTreeNode(
				Plug *plug,
				const void *value
			)
				{
					return
						new TreeNodeOf<V>(
							this,
							plug,
							*Cast_Pointer(const V*, value)
						);
				}
		int
			CompareTreeNodes(
				TreeNode *link1, 
				TreeNode *link2
			);
		int
			CompareValueToTreeNode(
				const void *value,
				TreeNode *link
			);
	};

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~ TreeOf templates ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

	template <class T, class V>
		TreeOf<T, V>::TreeOf(
			Node *node,
			bool has_unique_entries
		):
			Tree(
				node,
				has_unique_entries
			)
	{
	}

	template <class T, class V>
		TreeOf<T, V>::~TreeOf()
	{
	}

	template <class T, class V> int
		TreeOf<T, V>::CompareTreeNodes(
			TreeNode *node1,
			TreeNode *node2
		)
	{
      V *ptr1 = Cast_Object(TreeNodeOf<V>*, node1)->GetValuePointer();
      V *ptr2 = Cast_Object(TreeNodeOf<V>*, node2)->GetValuePointer();

      Check_Pointer(ptr1);
      Check_Pointer(ptr2);

   	if (*ptr1 == *ptr2)
      	return 0;
      else
      	return ((*ptr1 > *ptr2) ? 1 : -1);
	}

	template <class T, class V> int
		TreeOf<T, V>::CompareValueToTreeNode(
			const void *value,
			TreeNode *node
		)
	{
		Check_Pointer(value);

		V *ptr = Cast_Object(TreeNodeOf<V>*, node)->GetValuePointer();
      Check_Pointer(ptr);

      if (*Cast_Pointer(const V*, value) == *ptr)
      	return 0;
      else
      	return (*Cast_Pointer(const V*, value) > *ptr) ? 1 : -1;
	}

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~ TreeIterator ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

	class TreeIterator:
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
		explicit TreeIterator(Tree *tree);
		Iterator*
			MakeClone();
		~TreeIterator();
		void
			TestInstance();

		//
		//--------------------------------------------------------------------
		// Iterator methods (see Iterator for full listing)
		//--------------------------------------------------------------------
		//
		void
			First();
		void
			Next();
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
			GetCurrentImplementation();
		Plug*
			FindImplementation(const void *value);

	protected:
		//
		//--------------------------------------------------------------------
		// Protected data
		//--------------------------------------------------------------------
		//
		TreeNode
			*currentNode;

	private:
		//
		//--------------------------------------------------------------------
		//--------------------------------------------------------------------
		// Private interface
		//--------------------------------------------------------------------
		//--------------------------------------------------------------------
		//
		void
			Last();				 // Marked as private so that clients do not use it
		void
			Previous();			 // Marked as private so that clients do not use it

		void
			ReceiveMemo(
				IteratorMemo memo,
				void *content
			);
	};

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ TreeIteratorOf ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

	template <class T, class V> class TreeIteratorOf:
		public TreeIterator
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
		explicit TreeIteratorOf(TreeOf<T, V> *tree);
		Iterator*
			MakeClone();
		~TreeIteratorOf();

		//
		//--------------------------------------------------------------------
		// Iterator methods (see Iterator for full listing)
		//--------------------------------------------------------------------
		//
		T
			ReadAndNext()
				{return (T)ReadAndNextImplementation();}
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
         	{return Cast_Object(TreeNodeOf<V>*, currentNode)->GetValue();}
	};

	//~~~~~~~~~~~~~~~~~~~~~~~~~ TreeIteratorOf templates ~~~~~~~~~~~~~~~~~~~~~~~

	template <class T, class V>
		TreeIteratorOf<T, V>::TreeIteratorOf(TreeOf<T, V> *tree):
			TreeIterator(tree)
	{
	}

	template <class T, class V> Iterator*
		TreeIteratorOf<T, V>::MakeClone()
	{
		return new TreeIteratorOf<T, V>(*this);
	}

	template <class T, class V>
		TreeIteratorOf<T, V>::~TreeIteratorOf()
	{
	}

}
