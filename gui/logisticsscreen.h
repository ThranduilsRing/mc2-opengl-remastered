#ifndef LOGISTICSSCREEN_H
#define LOGISTICSSCREEN_H
/*************************************************************************************************\
LogisticsScreen.h			: Interface for the LogisticsScreen component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include"asystem.h"
#include <string>
#include <vector>
//*************************************************************************************************
class FitIniFile;
class aObject;
class aRect;
class aText;
class aAnimButton;
class aButton;
class aEdit;
class aAnimObject;
namespace UiDefs { class GameOSPage; }
/**************************************************************************************************
CLASS DESCRIPTION
LogisticsScreen:
**************************************************************************************************/
class LogisticsScreen : public aObject
{
	public:

	enum Status
	{
		RUNNING = 0,
		NEXT = 1,
		PREVIOUS = 2,
		DONE = 3,
		PAUSED = 4,
		UP,
		DOWN,
		YES,
		NO,
		MAINMENU,
		RESTART,
		MULTIPLAYERRESTART,
		SKIPONENEXT,
		SKIPONEPREVIOUS,
		FADEDOWN,
		FADEUP,
		READYTOLOAD,
		GOTOSPLASH
	};

	LogisticsScreen();
	virtual ~LogisticsScreen();
	LogisticsScreen( const LogisticsScreen& src );
	LogisticsScreen& operator=( const LogisticsScreen& src );

	void init(FitIniFile& file, const char* staticName, const char* textName, const char* rectName,
					  const char* buttonName, const char* editName = "Edit",
					  const char* animObjectName = "AnimObject", DWORD neverFlush = 0x2 );

	// Load only the v2 FIT for the given legacy FIT path (resolves the replacement
	// via UiDefs::replacementPathForLegacyFit).  Returns true if a replacement was
	// found and loaded.  No legacy arrays (statics/rects/buttons/textObjects) are
	// allocated.  Use instead of init() when ImGui is the sole UI layer.
	bool tryInitDefsOnly(const char* legacyFitPath);

	virtual void update();
	virtual void render();

	virtual void begin();
	virtual void end(){}

	virtual void render( int xOffset, int yOffset );

	// Renders the full legacy element set (statics, rects, buttons, texts)
	// without checking for a data/defs v2 page.  Used by MainMenu to render
	// the planet background statics from mcl_sp.fit when the solo-mission
	// v2 FIT has also been loaded into this screen's defsUiPage.
	void renderLegacy();

	// True when this screen renders through a data/defs UI Editor FIT page
	// (the ImGui HUD path) instead of the legacy GameOS widget set.  Callers
	// use this to suppress legacy GameOS draws that would otherwise be
	// composited UNDER the ImGui layer and double-drawn.
	bool hasDefsUiPage() const;

	// True when every legacy animObject's animation has finished (or the
	// screen has none).  Sequenced intros gate on this rather than
	// animObjects[0] alone, so closing fades that outlast the first
	// animation still play to completion.
	bool allAnimObjectsDone() const;

	// Forward list data into the defsUiPage for screens that embed a GuiList
	// inside their main FIT.  No-ops when hasDefsUiPage() is false.
	bool setDefsListItems(const std::string& key, const std::vector<std::string>& items);
	// Optional per-item text colors (ARGB), parallel to the items pushed above.
	bool setDefsListItemColors(const std::string& key, const std::vector<unsigned int>& colors);
	// Screen-rect of a defs element + an ImGui-layer rect draw, for overlaying dynamic
	// content (mission-briefing objective markers) on top of a defs image, aligned to it.
	// UI-LAYER-CONTRACT-2: bridge-render legacy widgets the defs page does not mirror.
	void renderUncoveredLegacyWidgets( int xOffset, int yOffset );
	bool getDefsElementScreenRect(const std::string& key, float& x, float& y, float& w, float& h);
	void drawDefsRect(float x, float y, float w, float h, unsigned int color, bool filled);

	// GuiSlider value (0..maxValue) — seed from prefs, read back the user's drag.
	bool setDefsSliderValue(const std::string& key, int value);
	int  getDefsSliderValue(const std::string& key) const;
	int  getDefsListItemCount(const std::string& key) const;
	int  getDefsListSelection(const std::string& key) const;
	void setDefsListSelection(const std::string& key, int index);

	// True when defsUiPage has at least one visible GuiEditBox; callers use
	// this to suppress legacy aEdit renders that would otherwise double-draw.
	bool hasDefsEditBox() const;

	// Forward edit-box data into the defsUiPage for screens that embed a
	// GuiEditBox inside their main FIT.  No-ops when hasDefsUiPage() is false.
	bool getDefsEditText(const std::string& key, std::string& text) const;
	bool setDefsEditText(const std::string& key, const std::string& text);
	bool isDefsEditBoxFocused(const std::string& key) const;
	bool isAnyDefsEditBoxFocused() const;
	void requestDefsEditFocus(const std::string& key);

	// Override the display text of any GuiText or GuiButton element at runtime.
	// Used to switch Save/Load dialog title and button label dynamically.
	bool setDefsElementText(const std::string& key, const std::string& text);

	// Runtime show/hide of a defs element by key (mirrors legacy statics[].
	// showGUIWindow onto the equivalent defs element).
	bool setDefsElementVisible(const std::string& key, bool visible);

	// Sets the texture path on a GuiImage element at runtime.
	bool setDefsElementTexture(const std::string& key, const std::string& texturePath);

	// Inject a runtime texture node (mission tac-map) into an image element.
	bool setDefsElementTextureNode(const std::string& key, long textureNode);
	bool setDefsElementGosTexture(const std::string& key, unsigned int gosHandle);

	// File-backed image with explicit texel sub-rect + destination rect (weapon icon).
	bool setDefsElementImageRegion(const std::string& key, const std::string& texturePath,
		int uvX, int uvY, int uvW, int uvH, int dstX, int dstY, int dstW, int dstH,
		bool legacyUvSpace = false);

	long getStatus();

	aButton* getButton( long who );
	aButton* getButtonByIndex( long index );
	aRect* getRect( long who );

	virtual void  moveTo( long xPos, long yPos );
	virtual void  move( long xPos, long yPos );


	bool	inside( long x, long y);

	void	beginFadeIn( float fNewTime ){ fadeInTime = fNewTime; fadeOutTime = fadeTime = 0.f; }
	void	beginFadeOut( float fNewTime ) { fadeInTime = 0.f; fadeOutTime = fNewTime; fadeTime = 0.f; }


	void	clear(); // remove everything


	
	aObject*			statics;
	aRect*				rects;
	long				rectCount;
	long				staticCount;

	aText*				textObjects;
	long				textCount;

	aAnimButton*			buttons;
	long				buttonCount;

	aEdit*				edits;
	long				editCount;

	aAnimObject*		animObjects;
	long				animObjectsCount;

	float				fadeInTime;
	float				fadeOutTime;
	float				fadeTime;


	protected:

	long				status;
	long				fadeOutMaxColor;

	long				helpTextArrayID;
	// When non-empty, setDefsElementText(defsHelpTextKey, ...) is called
	// alongside textObjects[helpTextArrayID].setText() so the defsUiPage
	// element stays in sync with the legacy textObject.
	std::string			defsHelpTextKey;
	private:

	void copyData( const LogisticsScreen& );
	void destroy();

	UiDefs::GameOSPage* defsUiPage;




};


//*************************************************************************************************
#endif  // end of file ( LogisticsScreen.h )
