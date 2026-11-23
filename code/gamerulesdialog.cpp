#define GAMERULESDIALOG_CPP
// GameRulesDialog: see gamerulesdialog.h. Rows are generated from mc2::rules::kRules; the page (mcl_gamerules.fit) supplies kPageRows slots.
#include "gamerulesdialog.h"
#include <cctype>
#include <string>
#include "mclib.h"
#include "platform_windows.h"
#include "abutton.h"
#include "inifile.h"
#include "gamesound.h"
#include "sounds.h"
#include "utils/mc2log.h"

std::unique_ptr<GameRulesDialog> GameRulesDialog::s_instance = nullptr;

namespace {
constexpr unsigned long kRowBase = 100;     // value button of row i sends 100 + i
constexpr unsigned long kAccept = 90, kDefaults = 91, kPreset = 92, kBack = 93;

std::string sliderKey( std::size_t row ) { return std::string( "game.mcl_gamerules.slider.row" ).append( std::to_string( row ) ); }
std::string rowKey( std::string_view kind, std::size_t row ) { return std::string( "game.mcl_gamerules." ).append( kind ).append( ".row" ).append( std::to_string( row ) ); }

// "PilotSkillWeight" -> "PILOT SKILL WEIGHT", "CBillMultiplier" -> "C BILL MULTIPLIER".
std::string labelFor( std::string_view key )
{
	std::string out;
	for ( std::size_t i = 0; i < key.size(); ++i )
	{
		const char c = key[i];
		const bool up = std::isupper( static_cast<unsigned char>( c ) ) != 0;
		if ( i && up )
		{
			const bool prevLower = std::islower( static_cast<unsigned char>( key[i - 1] ) ) != 0;
			const bool nextLower = i + 1 < key.size() && std::islower( static_cast<unsigned char>( key[i + 1] ) ) != 0;
			if ( prevLower || nextLower ) out += ' ';
		}
		out += static_cast<char>( std::toupper( static_cast<unsigned char>( c ) ) );
	}
	return out;
}

std::string valueText( const mc2::rules::GameRules& g, const mc2::rules::Rule& r )
{
	const long v = mc2::rules::getLong( g, r );
	if ( r.kind == mc2::rules::Kind::Enum ) return std::string( r.labels[v - r.lo] );	// the default option's label already says "MC2 Default"
	std::string text;
	if ( r.kind == mc2::rules::Kind::Bool ) text = v ? "On" : "Off";
	else text = std::to_string( v ) + ( r.kind == mc2::rules::Kind::Percent ? "%" : "" );
	return v == mc2::rules::getLong( mc2::rules::GameRules{}, r ) ? "MC2 Default (" + text + ")" : text;
}

// Rules no sim/economy code reads yet (the drop-limit and combat lanes consume them later): shown greyed, "(not yet active)".
bool ruleActive( std::string_view key )
{
	for ( std::string_view k : { "DropLimitMode", "DropWeightPurchase", "DropWeightStep", "Heat", "AmmoExplosions" } )
		if ( k == key ) return false;
	return true;
}
constexpr unsigned int kActiveColor = 0xffc66600, kGreyColor = 0xff6b6b6b, kActiveValueColor = 0xff5C96C2;
}  // namespace

GameRulesDialog::GameRulesDialog() = default;
GameRulesDialog::~GameRulesDialog() = default;

bool GameRulesDialog::init()
{
	if ( s_instance ) return true;
	if ( mc2::rules::kRuleCount > kPageRows )
	{
		MC2_LOG( Ui, Error, "[GAME_RULES] the descriptor table has {} rules but mcl_gamerules.fit has {} rows; dialog disabled", mc2::rules::kRuleCount, kPageRows );
		return false;
	}
	auto d = std::make_unique<GameRulesDialog>();
	// Defs-only screen: the legacy FIT name only selects the page (data/defs/ui/packages/default/game/<stem>.fit).
	if ( !d->tryInitDefsOnly( "mcl_gamerules.fit" ) )
	{
		MC2_LOG( Ui, Warn, "[GAME_RULES] mcl_gamerules defs page not available; the rules dialog is skipped" );
		return false;
	}
	s_instance = std::move( d );
	return true;
}

void GameRulesDialog::begin( const mc2::rules::GameRules& start )
{
	working = start;
	focusRow = -1;
	moveFocus( 1 );
	status = RUNNING;
	bDone = false;
	refreshRows();
	soundSystem->playDigitalSample( LOG_MAINMENUBUTTON );
	gos_KeyboardFlush();
}

void GameRulesDialog::refreshRows()
{
	setDefsElementText( "game.mcl_gamerules.text.title", "GAME RULES" );
	for ( std::size_t i = 0; i < kPageRows; ++i )
	{
		const bool used = i < mc2::rules::kRuleCount;
		setDefsElementVisible( rowKey( "text", i ), used );
		const bool slider = used && mc2::rules::isSlider( mc2::rules::kRules[i] );
		setDefsElementVisible( rowKey( "button", i ), used && !slider );
		setDefsElementVisible( sliderKey( i ), slider );
		setDefsElementVisible( rowKey( "value", i ), slider );
		if ( !used ) continue;
		const mc2::rules::Rule& r = mc2::rules::kRules[i];
		if ( slider )
		{
			lastSlider[i] = mc2::rules::sliderFromValue( r, mc2::rules::getLong( working, r ) );
			setDefsSliderValue( sliderKey( i ), static_cast<int>( lastSlider[i] ) );
			const long v = mc2::rules::getLong( working, r );
			setDefsElementText( rowKey( "value", i ), std::to_string( v ) + ( r.kind == mc2::rules::Kind::Percent ? "%" : "" ) );
			setDefsElementTextColor( rowKey( "value", i ), ruleActive( r.key ) ? kActiveValueColor : kGreyColor );
		}
		const bool active = ruleActive( r.key );
		setDefsElementText( rowKey( "text", i ), labelFor( r.key ) + ( active ? "" : "  (not yet active)" ) );
		setDefsElementText( rowKey( "button", i ), valueText( working, r ) );
		setDefsElementTextColor( rowKey( "text", i ), active ? kActiveColor : kGreyColor );
		setDefsElementTextColor( rowKey( "button", i ), active ? kActiveValueColor : kGreyColor );
	}
}

void GameRulesDialog::cycleRow( std::size_t row )
{
	if ( row >= mc2::rules::kRuleCount ) return;
	const mc2::rules::Rule& r = mc2::rules::kRules[row];
	long v = mc2::rules::getLong( working, r );
	long step = 1;
	if ( r.kind == mc2::rules::Kind::Percent ) step = 25;
	else if ( r.kind == mc2::rules::Kind::Int && r.hi - r.lo >= 20 ) step = 5;
	v = v + step > r.hi ? r.lo : v + step;
	mc2::rules::setLong( working, r, v );
	soundSystem->playDigitalSample( LOG_MAINMENUBUTTON );
}

int GameRulesDialog::handleMessage( unsigned long, unsigned long who )
{
	if ( bDone ) return 0;
	if ( who >= kRowBase && who < kRowBase + mc2::rules::kRuleCount )
	{
		if ( !mc2::rules::isSlider( mc2::rules::kRules[who - kRowBase] ) ) cycleRow( who - kRowBase );
	}
	else if ( who == kDefaults )
		working = mc2::rules::GameRules{};
	else if ( who == kPreset )
		working = mc2::rules::battleTechPreset();
	else if ( who == kAccept || who == YES )
	{
		status = YES;
		bDone = true;
	}
	else if ( who == kBack || who == NO )
	{
		status = NO;
		bDone = true;
	}
	refreshRows();
	return 0;
}

// The slider rows: the page's GuiSlider (drag + click) moved -> apply the nearest notch and put the slider on it.
void GameRulesDialog::pollSliders()
{
	for ( std::size_t i = 0; i < mc2::rules::kRuleCount; ++i )
	{
		const mc2::rules::Rule& r = mc2::rules::kRules[i];
		if ( !mc2::rules::isSlider( r ) ) continue;
		const long pos = getDefsSliderValue( sliderKey( i ) );
		if ( pos == lastSlider[i] ) continue;
		focusRow = static_cast<int>( i );
		const long v = mc2::rules::valueFromSlider( r, pos );
		if ( v != mc2::rules::getLong( working, r ) )
		{
			mc2::rules::setLong( working, r, v );
			soundSystem->playDigitalSample( LOG_MAINMENUBUTTON );
		}
		refreshRows();	// re-snaps the slider and the value text
	}
}

// Up/Down (and Tab) pick which slider Left/Right move.
void GameRulesDialog::moveFocus( int dir )
{
	const int n = static_cast<int>( mc2::rules::kRuleCount );
	for ( int k = 1; k <= n; ++k )
	{
		const int i = ( ( focusRow < 0 ? ( dir > 0 ? -1 : 0 ) : focusRow ) + dir * k % n + n ) % n;
		if ( mc2::rules::isSlider( mc2::rules::kRules[i] ) ) { focusRow = i; return; }
	}
}

void GameRulesDialog::update()
{
	if ( bDone ) return;
	userInput->mouseOn();
	userInput->setMouseCursor( mState_NORMAL );
	LogisticsScreen::update();
	pollSliders();
	if ( focusRow >= 0 )
	{
		const mc2::rules::Rule& fr = mc2::rules::kRules[focusRow];
		const int dir = gos_GetKeyStatus( KEY_LEFT ) == KEY_RELEASED ? -1 : ( gos_GetKeyStatus( KEY_RIGHT ) == KEY_RELEASED ? 1 : 0 );
		if ( dir )
		{
			mc2::rules::setLong( working, fr, mc2::rules::nudgeValue( fr, mc2::rules::getLong( working, fr ), dir ) );
			soundSystem->playDigitalSample( LOG_MAINMENUBUTTON );
			refreshRows();
		}
		if ( gos_GetKeyStatus( KEY_UP ) == KEY_RELEASED ) moveFocus( -1 );
		else if ( gos_GetKeyStatus( KEY_DOWN ) == KEY_RELEASED || gos_GetKeyStatus( KEY_TAB ) == KEY_RELEASED ) moveFocus( 1 );
	}
	if ( gos_GetKeyStatus( KEY_RETURN ) == KEY_RELEASED ) handleMessage( 0, kAccept );
	else if ( gos_GetKeyStatus( KEY_ESCAPE ) == KEY_RELEASED ) handleMessage( 0, kBack );
}

void GameRulesDialog::render()
{
	gos_SetHudScaleExempt( true );
	GUI_RECT rect = { 0, 0, Environment.screenWidth, Environment.screenHeight };
	drawRect( rect, 0xaf000000 );
	LogisticsScreen::render( 0, 0 );
	// Default mark (orange tick above the groove) and the keyboard focus outline, per slider row.
	for ( std::size_t i = 0; i < mc2::rules::kRuleCount; ++i )
	{
		const mc2::rules::Rule& r = mc2::rules::kRules[i];
		float x, y, w, h;
		if ( !mc2::rules::isSlider( r ) || !getDefsElementScreenRect( sliderKey( i ), x, y, w, h ) ) continue;
		const float thumbW = h * 0.55f > 8.0f ? h * 0.55f : 8.0f;
		const float frac = static_cast<float>( mc2::rules::sliderFromValue( r, mc2::rules::getLong( mc2::rules::GameRules{}, r ) ) ) / mc2::rules::kSliderRes;
		const float tickW = h * 0.12f > 2.0f ? h * 0.12f : 2.0f;
		drawDefsRect( x + frac * ( w - thumbW ) + ( thumbW - tickW ) * 0.5f, y - h * 0.22f, tickW, h * 0.3f, 0xffc66600, true );
		if ( static_cast<int>( i ) == focusRow ) drawDefsRect( x - 2.0f, y - 2.0f, w + 4.0f, h + 4.0f, 0xff5C96C2, false );
	}
	gos_SetHudScaleExempt( false );
}
