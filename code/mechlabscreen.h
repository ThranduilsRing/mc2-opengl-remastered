#ifndef MECHLABSCREEN_H
#define MECHLABSCREEN_H
#include <memory>
#include <optional>
/*************************************************************************************************\
MechLabScreen.h			: Interface for the MechLabScreen component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#ifndef LOGISTICSSCREEN_H
#include"logisticsscreen.h"
#endif

#ifndef ATTRIBUTEMETER_H
#include"attributemeter.h"
#endif

#ifndef COMPONENTLISTBOX_H
#include"componentlistbox.h"
#endif

#ifndef SIMPLECAMERA_H
#include"simplecamera.h"
#endif

inline constexpr int MECH_LAB_ATTRIBUTE_METER_COUNT = 9;
class FitIniFile;
class LogisticsVariant;
class LogisticsComponent;
class LogisticsVariantDialog;
class LogisticsAcceptVariantDialog;
//*************************************************************************************************

/**************************************************************************************************
CLASS DESCRIPTION
MechLabScreen:
**************************************************************************************************/
class MechLabScreen: public LogisticsScreen
{
	public:

		MechLabScreen();
		virtual ~MechLabScreen();

		int init( FitIniFile& );

		virtual void begin();
		virtual void end();
		virtual void update();
		virtual void render(int xOffset, int yOffset);

		virtual int	handleMessage( unsigned long, unsigned long );

		void	setComponent( LogisticsComponent* pComponent, bool bMessageFromLB = 0 );
		int		addComponent( LogisticsComponent* pComponent, long& x, long& y );
		void	beginDrag( LogisticsComponent* pComponent );
		void	endDrag( );

		int		canAddComponent( LogisticsComponent* pComponent );
		void inspectComponent( LogisticsComponent* c );

		static MechLabScreen* instance() { return s_instance;}

		bool canRemoveComponent( LogisticsComponent* pComp );


	private:

		MechLabScreen( const MechLabScreen& src );
		MechLabScreen& operator=( const MechLabScreen& echLabScreen );
		static MechLabScreen*	s_instance;
		
		static std::unique_ptr<LogisticsVariantDialog> saveDlg;
		static std::unique_ptr<LogisticsAcceptVariantDialog> acceptDlg;

		LogisticsVariantDialog*			pCurDialog;

		LogisticsVariant*	pVariant;
		LogisticsComponent*	pSelectedComponent;
		long				selI;
		long				selJ;

		ComponentIconListBox	componentListBox;
		aComboBox				variantList;

		AttributeMeter attributeMeters[MECH_LAB_ATTRIBUTE_METER_COUNT];

		ComponentIcon componentIcons[128];
		long				componentCount;

		aObject				payloadIcon;

		ComponentIcon dragIcon;

		LogisticsComponent*	pDragComponent;
		LogisticsComponent*	pCurComponent;
		LogisticsComponent* pInspectedComponent = nullptr;

		void showJumpJetItems( bool bShow );

		void updateDiagram();

		float originalCost;

		aObject				selRects[2][5];
		aObject				selJumpJetRect;
		aObject*			selRect;
		
		bool				bSaveDlg;
		bool				bDragLeft ;

		SimpleCamera		mechCamera;	// MECHLAB-3D-1: the posed 3D mech behind the weapon grid (replaces the Static50-53 silhouette)

		std::string			varName;

		static				GUI_RECT	sensorRects[4];
		static				long		sensorHelpIDs[4];

		float				countDownTime;
		float				curCount;
		float				previousAmount;
		float				oldCBillsAmount;

		float				oldHeat;
		float				heatTime;
		bool				flashHeatRange;

		float				oldArmor;
		float				newArmor;
		float				armorTime;

		bool				bErrorDlg;


		// MECHLAB-LOC-1: location view (package chassis); layout + hit rects in mechlab_loc_layout.h
		bool	locationViewActive() const;
		void	updateLocationInput();
		void	drawLocationView( float tbSx, float tbSy, float tbOx, float tbOy, int xOffset, int yOffset );
		struct PlacedAt { long x = -1; long y = -1; };
		std::optional<PlacedAt>	placeInPanel( LogisticsComponent* pComponent, int panel );
		void	endDragLocation();
		void	setLocationView( bool on );

		// HELPER FUNCTIONS
		void getMouseDiagramCoords( long& x, long& y );
		void getMouseDiagramCoords( long screenX, long screenY, long& x, long& y );

		void	diagramToScreen( long i, long j, long& x, long& y  );
		int		selectFirstDiagramComponent();
		int		selectFirstLBComponent(); 
		void	updateDiagramInput();
		void	swapVariant();
		void	updateHeatMeter();
		void	updateArmorMeter();
		void	removeComponent( long i, long j );



};


//*************************************************************************************************
#endif  // end of file ( MechLabScreen.h )
