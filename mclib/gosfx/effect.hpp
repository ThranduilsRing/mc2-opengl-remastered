//==========================================================================//
// File:	 gosFX_Effect.hpp												//
// Contents: Base Effect Component											//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#pragma once

#include"gosfx.hpp"
#include<mlr/mlr.hpp>
#include<memory>
#include<vector>

namespace MidLevelRenderer
{
	class MLRState;
	class MLRClipper;
	class MLRClippingState;
}

namespace gosFX
{
	class Effect__ClassData;

	//############################################################################
	//####################################  Event  ###############################
	//############################################################################

	// One child spawn of an effect spec. The spec owns its events (Effect__Specification::m_events); an Event is no longer a
	// Stuff::Plug.
	class Event
	{
	public:
		Event() = default;
		Event(const Event& event);

		// Check_Object/Register_Object hook (armor builds); an Event has no class data to verify.
		void TestInstance() const {}

		static Event*
			Make(
				Stuff::MemoryStream *stream,
				int gfx_version
			);

		void
			Save(Stuff::MemoryStream *stream);

		Stuff::Scalar
			m_time;
		unsigned
			m_flags,
			m_effectID;
		glm::mat4
			m_localToParent;

	protected:
		Event(
			Stuff::MemoryStream *stream,
			int gfx_version
		);
	};

	//############################################################################
	//########################  Effect__Specification  #############################
	//############################################################################

	class Effect__Specification
		#if defined(_ARMOR)
			: public Stuff::Signature
		#endif
	{
	//----------------------------------------------------------------------
	// Constructors/Destructors
	//
	protected:
		Effect__Specification(
			Stuff::RegisteredClass::ClassID class_id,
			Stuff::MemoryStream *stream,
			int gfx_version
		);

	public:
		Effect__Specification(Stuff::RegisteredClass::ClassID class_id=gosFX::EffectClassID);
		virtual ~Effect__Specification();

		static Effect__Specification*
			Create(
				Stuff::MemoryStream *stream,
				int gfx_version
			);

		using Factory = Effect__Specification*(*)(Stuff::MemoryStream *stream, int gfx_version);

		static Effect__Specification*
			Make(
				Stuff::MemoryStream *stream,
				int gfx_version
			);

		virtual void
			Save(Stuff::MemoryStream *stream);

		virtual void 
			BuildDefaults();
	
		virtual bool 
			IsDataValid(bool fix_data=false);

		Stuff::RegisteredClass::ClassID
			GetClassID()
				{Check_Object(this); return m_class;}

		virtual void
			Copy(Effect__Specification *spec);

		Stuff::MString
			m_name;
		unsigned
			m_effectID;

	protected:
		Stuff::RegisteredClass::ClassID
			m_class;

	//----------------------------------------------------------------------
	// Events
	//
	public:
		// In file order after a load (a file is not guaranteed to be sorted); AdoptEvent keeps the time order it is given.
		std::vector<std::unique_ptr<Event>>
			m_events;

		// Takes ownership: inserts before the first event later than `event` (linear, so equal times keep adoption order).
		void
			AdoptEvent(Event *event);

	//-------------------------------------------------------------------------
	// FCurves
	//
	public:
		ConstantCurve
			m_lifeSpan;
		SplineCurve
			m_minimumChildSeed,
			m_maximumChildSeed;

	//----------------------------------------------------------------------
	// States
	//
	public:
		MidLevelRenderer::MLRState
			m_state;

	//----------------------------------------------------------------------
	// Testing
	//
	public:
		void
			TestInstance() const
				{}
	};

	//############################################################################
	//###############################  Effect  ###################################
	//############################################################################

	class Effect;
	// The owner of a live effect: a parent effect for its children, the game object or appearance for a top-level one.
	using EffectPtr = std::unique_ptr<Effect>;

	class Effect:
		public Stuff::Node
	{
		friend class EffectCloud;

	//----------------------------------------------------------------------------
	// Types
	//
	public:
		struct ExecuteInfo
		{
			Stuff::Time
				m_time;
			Stuff::Scalar
				m_seed,					// 0 <= m_seed <= 1
				m_age,
				m_ageRate;
			const glm::mat4
				*m_parentToWorld;
			mc2::math::Obb
				*m_bounds;

			ExecuteInfo(
				Stuff::Time time,
				const glm::mat4 *parent_to_world,
				mc2::math::Obb *bounds,
				Stuff::Scalar seed = -1.0f
			)
				{
					m_time = time; m_seed = seed;
					m_parentToWorld = parent_to_world;
					m_bounds = bounds; m_age = -1.0f; m_ageRate = -1.0f;
				}

			void TestInstance() const
				{}

		private:
			ExecuteInfo(
				Stuff::Scalar time,
				const glm::mat4 *parent_to_world,
				mc2::math::Obb *bounds,
				Stuff::Scalar seed = -1.0f
			);
		};

		struct DrawInfo
		{
			const glm::mat4 *m_parentToWorld;
			MidLevelRenderer::MLRClippingState m_clippingFlags;
			MidLevelRenderer::MLRState m_state;
			MidLevelRenderer::MLRClipper *m_clipper;

			void TestInstance() const
				{}
		};

		using Specification = Effect__Specification;
		using ClassData = Effect__ClassData;

	//----------------------------------------------------------------------------
	// Initialization
	//
	public:
		static void InitializeClass();
		static void	TerminateClass();
		static ClassData
			*DefaultData;

	//----------------------------------------------------------------------------
	// Constructors/Destructors
	//
	protected:
		Effect(
			ClassData *class_data,
			Specification *spec,
			unsigned flags
		);

	public:
		~Effect();

		using Factory = Effect*(*)(Specification *spec, unsigned flags);

		static Effect*
			Make(
				Specification *spec,
				unsigned flags
			);

		Specification*
			GetSpecification()
				{Check_Object(this); return m_specification;}

	protected:
		Specification
			*m_specification;

	//----------------------------------------------------------------------------
	// Events
	//
	protected:
		// The parent owns every child it spawned, in spawn order (Execute, Draw and Kill visit them in that order).
		std::vector<EffectPtr>
			m_children;
		// Index of the next event of m_specification->m_events to fire; at or past the end, none are left.
		size_t
			m_event = 0;

	//----------------------------------------------------------------------------
	// Testing
	//
	public:
		void
			TestInstance() const;

	//----------------------------------------------------------------------------
	// API
	//
	public:
		virtual void Start(ExecuteInfo *info);
		void Stop();
		virtual bool Execute(ExecuteInfo *info);
		virtual void Kill();
		virtual void Draw(DrawInfo *info);
		virtual bool HasFinished();

		static constexpr int ExecuteFlag = 1;
		static constexpr int LoopFlag = 2;
		static constexpr int LocalSpaceSimulationMode = 0;
		static constexpr int DynamicWorldSpaceSimulationMode = 4;
		static constexpr int StaticWorldSpaceSimulationMode = 8;
		static constexpr int ParentSimulationMode = 12;
		static constexpr int SimulationModeMask = 12;

		static glm::vec3
			s_ether;
		static glm::vec3
			s_gravity;

	public:
		void
			SetExecuteOn()
				{Check_Object(this); m_flags |= ExecuteFlag;}
		void
			SetExecuteOff()
				{Check_Object(this); m_flags &= ~ExecuteFlag;}
		bool
			IsExecuted()
				{Check_Object(this); return (m_flags&ExecuteFlag) != 0;}

		void
			SetLoopOn()
				{Check_Object(this); m_flags |= LoopFlag;}
		void
			SetLoopOff()
				{Check_Object(this); m_flags &= ~LoopFlag;}
		bool
			IsLooped()
				{Check_Object(this); return (m_flags&LoopFlag) != 0;}

		void
			UseLocalSpaceSimulation()
				{Check_Object(this); m_flags &= ~SimulationModeMask;}
		void
			UseStaticWorldSpaceSimulation()
				{
					Check_Object(this); m_flags &= ~SimulationModeMask;
					m_flags |= StaticWorldSpaceSimulationMode;
				}
		void
			UseDynamicWorldSpaceSimulation()
				{
					Check_Object(this); m_flags &= ~SimulationModeMask;
					m_flags |= DynamicWorldSpaceSimulationMode;
				}
		int
			GetSimulationMode()
				{Check_Object(this); return m_flags & SimulationModeMask;}

		int
			GetSimulationFlags()
				{Check_Object(this); return m_flags;}

		glm::mat4
			m_localToWorld;

	protected:
		glm::mat4
			m_localToParent;
		Stuff::Time
			m_lastRan;
		Stuff::Scalar
			m_age,
			m_ageRate,
			m_seed;
		unsigned
			m_flags;
	};

	// Class-registry factories are stored as base-class function pointers. Registering a derived Make through a
	// function-pointer cast calls it through the wrong type (undefined); these thunks convert the arguments and the
	// result by value instead.
	template<class Result, class Spec> Spec firstArgOf(Result (*)(Spec, unsigned));
	template<auto Make> Effect* effectFactoryThunk(Effect::Specification *spec, unsigned flags)
	{
		return Make(static_cast<decltype(firstArgOf(Make))>(spec), flags);
	}
	template<auto Make> Effect::Specification* specificationFactoryThunk(Stuff::MemoryStream *stream, int gfx_version)
	{
		return Make(stream, gfx_version);
	}


	//##########################################################################
	//########################    Effect__ClassData    #########################
	//##########################################################################

	class Effect__ClassData:
		public Stuff::Plug::ClassData
	{
	//----------------------------------------------------------------------------
	//
	public:
		Effect__ClassData(
			Stuff::RegisteredClass::ClassID class_id,
			const char* class_name,
			Stuff::Plug::ClassData *parent_class,
			Effect::Factory effect_factory,
			Effect::Specification::Factory spec_factory
		):
			RegisteredClass__ClassData(class_id, class_name, parent_class),
			effectFactory(effect_factory),
			specificationFactory(spec_factory)
				{}

		Effect::Factory 
			effectFactory;
		Effect::Specification::Factory
			specificationFactory;

	//----------------------------------------------------------------------------
	//
	public:
		void TestInstance();
	};

}
