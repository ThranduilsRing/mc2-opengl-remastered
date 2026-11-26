#ifndef GAMERULESDIALOG_H
#define GAMERULESDIALOG_H
// The Game Rules dialog: one row per entry of the mc2::rules::kRules descriptor table (code/game_rules.h), no per-rule code.
// Reached from New Campaign (picker -> rules -> startNewCampaign). Page: data/defs/ui/packages/default/game/mcl_gamerules.fit
// (a defs-only screen: it has no legacy FIT). An enum/bool row's value button cycles the rule; a numeric row is a snapping slider (drag, click, left/right); ACCEPT / DEFAULTS / BATTLETECH PRESET / BACK.
#include <memory>
#include "logisticsdialog.h"
#include "game_rules.h"
#include "ascroll.h"

class GameRulesDialog : public LogisticsDialog
{
public:
	static GameRulesDialog* instance() { return s_instance.get(); }
	// Loads the defs page once. False when the defs UI is off or the page is missing (the caller then skips the dialog).
	static bool init();

	// Rows are filled from `start`; status is RUNNING until ACCEPT (YES) or BACK (NO).
	void begin( const mc2::rules::GameRules& start, bool campaignStart = false );
	virtual void update();
	virtual void render();
	virtual int handleMessage( unsigned long, unsigned long );

	const mc2::rules::GameRules& rules() const { return working; }

	GameRulesDialog();
	virtual ~GameRulesDialog();

	// Rows the page provides; the descriptor table may not outgrow it (checked at init).
	static constexpr std::size_t kPageRows = 11;

private:
	GameRulesDialog( const GameRulesDialog& ) = delete;
	GameRulesDialog& operator=( const GameRulesDialog& ) = delete;

	struct ModsPanel : LogisticsScreen {
		GameRulesDialog* owner = nullptr;
		int handleMessage(unsigned long, unsigned long who) override;
	} modsPanel;
	mcScrollBar modsScroll;
	int modOffset = 0, highlightedMod = 0;
	void refreshMods();
	void refreshRows();
	void cycleRow( std::size_t row );
	void pollSliders();
	void moveFocus( int dir );

	long lastSlider[kPageRows] = {};	// last slider position the dialog applied (the page slider is re-set to the snapped notch)
	int focusRow = -1;				// slider row the left/right keys move
	bool editMods = false;
	bool showFocus = false;			// the focus outline appears only after keyboard use

	mc2::rules::GameRules working;
	static std::unique_ptr<GameRulesDialog> s_instance;
};

#endif
