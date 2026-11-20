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
	if ( r.kind == mc2::rules::Kind::Bool ) return v ? "ON" : "OFF";
	if ( r.kind == mc2::rules::Kind::Enum ) return std::string( r.labels[v - r.lo] );
	return std::to_string( v ) + ( r.kind == mc2::rules::Kind::Percent ? "%" : "" );
}
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
		setDefsElementVisible( rowKey( "button", i ), used );
		if ( !used ) continue;
		const mc2::rules::Rule& r = mc2::rules::kRules[i];
		setDefsElementText( rowKey( "text", i ), labelFor( r.key ) );
		setDefsElementText( rowKey( "button", i ), valueText( working, r ) );
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
		cycleRow( who - kRowBase );
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

void GameRulesDialog::update()
{
	if ( bDone ) return;
	userInput->mouseOn();
	userInput->setMouseCursor( mState_NORMAL );
	LogisticsScreen::update();
	if ( gos_GetKeyStatus( KEY_RETURN ) == KEY_RELEASED ) handleMessage( 0, kAccept );
	else if ( gos_GetKeyStatus( KEY_ESCAPE ) == KEY_RELEASED ) handleMessage( 0, kBack );
}

void GameRulesDialog::render()
{
	gos_SetHudScaleExempt( true );
	GUI_RECT rect = { 0, 0, Environment.screenWidth, Environment.screenHeight };
	drawRect( rect, 0xaf000000 );
	LogisticsScreen::render( 0, 0 );
	gos_SetHudScaleExempt( false );
}
