//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef ASYSTEM_H
#define ASYSTEM_H

class aSystem;
class aCallback;
class aObject;
class aTitleWindow;
class InterfaceObject;

#include<gameos.hpp>

#include"estring.h"
#include"afont.h"
#include "asset_scale.h"

#define MAX_CHILDREN 64

#define aMSG_LEFTMOUSEDOWN              1
#define aMSG_MIDDLEMOUSEDOWN			2
#define aMSG_RIGHTMOUSEDOWN             3
#define aMSG_LEFTMOUSEUP                4
#define aMSG_MIDDLEMOUSEUP              5
#define aMSG_RIGHTMOUSEUP               6
#define aMSG_MOUSEMOVE                  7
#define aMSG_LEFTMOUSEDBLCLICK			16
#define aMSG_RIGHTMOUSEDBLCLICK			17
#define aMSG_LEFTMOUSEHELD				18
#define aMSG_RIGHTMOUSEHELD				19
#define aMSG_SCROLLUP		101
#define aMSG_SCROLLDOWN		102
#define aMSG_PAGEUP			103
#define aMSG_PAGEDOWN		104
#define aMSG_TRACKTAB		106
#define aMSG_SCROLLTO		107	//	sent by parent
#define aMSG_SCROLLPARENT	108	//	sent to parent
#define aMSG_SELECTME		109
#define aMSG_DONE			110
#define aMSG_BUTTONCLICKED			111
#define aMSG_SELCHANGED		112

typedef enum
{
	UNDEFINEDWINDOW = -1,
	GENERIC = 1,
	STATIC,
	SCROLLBAR,
	BUTTON,
	LISTBOX,
	COMBOBOX,
	NUMBER_OF_WINDOWTYPES
} WINDOW_ID;



// Error codes, local to this file...
#define	DUPLICATE_INSTANCE			-1
#define	FAILED_TO_CREATE_WINDOW		-2
#define	DISPLAY_MISMATCH			-3
#define INIT_FAILED					-4
#define	FAILED_TO_CREATE_SYSTEM		-5
#define	LOCK_FAILURE				-6
#define	UNLOCK_FAILURE				-7
#define	BLT_FAILURE					-8
#define FAILED_TO_ALLOCATE_PORT		-9
#define	USER_INIT_FAILED			-10

#include"utilities.h"

extern long helpTextID;
extern long helpTextHeaderID;



class aBaseObject
{
public:
	
	virtual void render(){}
	virtual void update(){}
};
// base class aObject definition
class aObject : public aBaseObject
{

public:
	aObject();
	virtual ~aObject();
	aObject( const aObject& src );
	aObject& operator=( const aObject& src );
	
	virtual long init(long xPos, long yPos, long w, long h);
	virtual void destroy();

	float		width() const;
	float		height() const;
	float		x() const;
	float		y() const ;
	
	virtual void	moveTo(long xPos, long yPos);
	virtual void	moveToNoRecurse(long xPos, long yPos);
	
	void		resize(long w, long h);
	void		addChild(aObject* c);
	void		removeChild(aObject* c);
	void		setParent(aObject* p);

	void		setTexture( const char* fileName );
	void		setTexture(unsigned long newHandle );
	void		setUVs( float u1, float v1, float u2, float v2 );
	void		setColor(uint32_t color, bool bRecurse = 0); // color the vertices

	void		init( FitIniFile* file, const char* block, DWORD neverFlush = 0x2 ); // for statics
	
	aObject*			getParent()
	{
		return pParent;			// No Need for this to be virtual!!!!!  Can now check if object has been deleted without crashing!
	}
	
	long				numberOfChildren() const;
	long				globalX() const;
	long				globalY() const;
	long				globalLeft() const { return globalX(); }
	long				globalTop() const { return globalY(); }
	long				globalRight() const;
	long				globalBottom() const;
	
	virtual aObject*	findObject(long xPos, long yPos);
	virtual int			handleMessage( unsigned long, unsigned long ){ return 0; }
	virtual bool		pointInside(long xPos, long yPos) const;
	bool				rectIntersect(long top, long left, long bottom, long right) const;
	bool				rectIntersect(const GUI_RECT& testRect) const;
	
	aObject*	children();
	aObject*	child(long w);

	
	virtual void		render();
	virtual void		render(long x, long y);
	virtual void		update();

	long getColor(){ return location[0].argb; }
		

	void		showGUIWindow(bool show) {showWindow = show;}
	bool		isShowing(void) const {return showWindow;}

	void				FillBox(short left, short top, short bottom, short right, char color);
	void				SetBit(long xpos, long ypos, char value);
	void				removeAllChildren( bool bDelete = 0);
	virtual void		move( float offsetX, float offsetY );
	virtual void		moveNoRecurse( float offsetX, float offsetY );

	void				setFileWidth( float newWidth ){ fileWidth = newWidth; }
	// setFileHeight: override the V-axis divisor independently of fileWidth.
	// Used by icon atlases that are taller than wide (e.g. 256x512 mech-icon
	// atlas) so V-coords are divided by the actual atlas height rather than
	// fileWidth.  If fileHeight is 0 (default), setUVs falls back to fileWidth
	// for both axes (backward-compatible with all existing callers).
	void				setFileHeight( float newHeight ){ fileHeight = newHeight; }
	float				getFileHeight() const { return fileHeight; }
	// getTextureHandle: read-only access to the loaded texture node id.  Lets a
	// caller (e.g. MechListBox::initIcon) query the atlas's logical size to set
	// fileHeight explicitly for a non-square atlas, instead of asystem auto-
	// detecting it for every atlas (which shifts UVs on unrelated GUI panels).
	unsigned long		getTextureHandle() const { return textureHandle; }
	int				getID() const { return ID; }
	void			setID(int newID) { ID = newID; }

	void			setHelpID( int newID ) { helpID = newID; }
	int				getHelpID() const { return helpID; }

	// data/defs UI bridge: while active, aObject::render routes its quad to
	// the ImGui HUD layer (GuiRuntime) instead of the GameOS draw path, with
	// coordinates scaled from legacy Environment space to the ImGui display.
	// Used by LogisticsScreen to draw live legacy animObjects (with their
	// real keyframe playback) on top of data/defs UI pages, which composite
	// AFTER all GameOS draws and would otherwise hide them.
	static void			beginGuiBridge(float scaleX, float scaleY);
	// UI-LAYER-CONTRACT-2: bridge with a display-pixel origin offset (the
	// 16:9 canvas pads). Plain beginGuiBridge keeps offset (0,0).
	static void			beginGuiBridge(float scaleX, float scaleY, float offX, float offY);
	// Computes the canvas-aware transform (display size + 16:9 canvas box)
	// and begins the bridge with it -- ALL legacy->ImGui bridge sites should
	// use this so bridged widgets land on the same canvas as the defs page.
	static void			beginGuiBridgeCanvas();
	// The canvas transform itself (scale legacy->display + pad origin), for
	// callers that composite manually (drawPreviewToPanel panel rects, text
	// bridge). Falls back to full-stretch when no canvas is active.
	static void			getCanvasTransform(float& sx, float& sy, float& ox, float& oy);
	static void			endGuiBridge();

	// Text-only bridge: while active, aText::render draws its label through the
	// ImGui TTF path (GuiRuntime::DrawUiText) instead of the GameOS bitmap font,
	// with coordinates scaled from legacy Environment space to the ImGui display.
	// Unlike beginGuiBridge it does NOT reroute aObject quads, so legacy widgets
	// (mech-bay deployment icons, mech-storage list items) keep drawing their art
	// on the GameOS layer while their text upgrades to crisp TTF on the HUD layer.
	static void			beginTextBridge(float scaleX, float scaleY, float fontScale = 1.0f, float offX = 0.0f, float offY = 0.0f);
	static void			endTextBridge();

	// Shared TTF-bridge draw for legacy text widgets (aText, aTextListItem, ...).
	// If a text/gui bridge is active, draws the label through GuiRuntime::DrawUiText
	// (crisp TTF, scaled legacy->display) and returns true; otherwise returns false
	// and the caller falls back to its GameOS bitmap-font path.  rect is the widget's
	// local rect in Environment space (x0,y0 = top-left, x1,y1 = bottom-right).
	static bool			renderTextBridged(aFont& font, const char* text,
										  float x0, float y0, float x1, float y1,
										  unsigned long argb, long alignment);


	float		left()
	{
		return x();
	}
	float		top()
	{
		return y();
	}
	float		right()
	{
		return x() + width();
	}

	float		bottom()
	{
		return y() + height();
	}

	enum class SrcRectSpace { ActualPixels, NominalPixels };

	void setAssetScale( const AssetScale::AssetKey& k,
	                    SrcRectSpace space = SrcRectSpace::NominalPixels )
	{
		assetKey     = k;
		srcRectSpace = space;
	}



protected:

	gos_VERTEX		location[4];

	unsigned long	textureHandle;
	float		fileWidth;
	float		fileHeight;  // V-axis divisor; 0 = use fileWidth (default, backward-compat)
	bool		showWindow;

	AssetScale::AssetKey	assetKey;
	SrcRectSpace			srcRectSpace = SrcRectSpace::ActualPixels;

	
	aObject*	pChildren[MAX_CHILDREN];
	long		pNumberOfChildren;
	aObject*	pParent;
	
	int			ID;

	void copyData( const aObject& src );

	long			helpHeader;
	long			helpID;


};


//class aRect : public aBaseObject
/* It may seem wasteful to derive from aObject instead of aBaseObject, but an aRect 
needs to able to be a child of an aObject. Perhaps bounding box and parent/child support
should be part of aBaseObject. */
class aRect : public aObject
{
public:

	aRect();
	virtual ~aRect(){}

	virtual void render();
	virtual void		render(long x, long y);

	virtual void init( FitIniFile* file, const char* blockName );

	GUI_RECT getGUI_RECT();
	GUI_RECT getGlobalGUI_RECT();

	bool	bOutline;

};

class aText : public aObject
{
public:

	aText();
	aText( const aText& src );
	aText& operator=( const aText& src );

	virtual ~aText();

	virtual void render();
	virtual void		render(long x, long y);
	

	void	init( FitIniFile* file, const char* header );

	void	setText( const EString& text );
	void		setText( long resID );

	EString		text;
	long			alignment; // left, right, ala GOS
	aFont		font;

	virtual bool		pointInside(long xPos, long yPos) const;

private:

	void CopyData( const aText& src );



};



#endif // ASYSTEM_H
