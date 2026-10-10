//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include"afont.h"
#include"mclib.h"
#include"platform_mbstring.h"
#include "Gates/gate.h"

aFont::aFont()
{
	gosFont = 0;
	fontName[0] = 0;
	resID = 0;
	size = 1;
}

// MC2_HUD_READABILITY_V1 (default OFF): data-first HUD readability.
// The in-mission HUD resolves its fonts through string-table resource IDs
// (aFont::init(long) -> loadFont -> cLoadString -> "<basename>.d3f[,size]"),
// so text size is baked into the D3F atlas that gets loaded — text code
// always renders at scale 1.0 (gos_TextSetAttributes Size is ignored by the
// renderer). This shim substitutes the next-size-up retail D3F atlas for the
// worst-offender HUD fonts at the single resolve choke point. Gate OFF =>
// zero behavior change (helper not consulted). No layout or hit-test change:
// only the glyph atlas swaps.
static bool mc2HudReadabilityV1()
{
	static int cached = -1;
	if (cached < 0)
	{
		cached = mc2::gate::get<"MC2_HUD_READABILITY_V1">();
	}
	return cached == 1;
}

// Returns the replacement font file for a HUD font resource ID, or NULL to
// keep the stock string. IDs from resource.h; stock string values from the
// shipped string table (test_scripts/res_conv/strings.res.cpp). Only retail
// atlases that exist in assets/graphics are targeted (agencyfb 11/14/17/20,
// arialnarrow 6/8/9). Chat (IDS_CHAT_FONT = ArialNarrow9) has no larger
// same-family atlas — left alone, deferred to the ImGui port slices.
static const char* mc2HudReadabilityFontOverride(long resourceID)
{
	switch (resourceID)
	{
	case 40024: return "arialnarrow9.d3f"; // IDS_HELP_FONT_1024      (ArialNarrow8)
	case 40025: return "arialnarrow9.d3f"; // IDS_OBJECTIVE_FONT_1024 (ArialNarrow8)
	case 40026: return "agencyfb17.d3f";   // IDS_INFONAME_FONT_1024  (AgencyFB14)
	case 40044: return "agencyfb17.d3f";   // IDS_COMPONENTS1024      (AgencyFB14)
	case 40035: return "agencyfb14.d3f";   // IDS_VEHICLEFONT1024     (AgencyFB11)
	case 40031: return "agencyfb14.d3f";   // IDS_PAUSEBUTTON800      (AgencyFB11)
	case 40017: return "agencyfb17.d3f";   // IDS_PAUSEDFONT_800      (AgencyFB14)
	case 43227: return "arialnarrow9.d3f"; // IDS_HOT_KEY_FONT        (ArialNarrow8)
	case 43232: return "agencyfb14.d3f";   // IDS_HOT_KEY_HEADER_FONT (AgencyFB11)
	default:    return 0;
	}
}

long aFont::init( const char* newFontName )
{
	destroy();
	gosFont = 0;

	strcpy( fontName, newFontName );
	char path[256];

	char* pStr = strstr( fontName, "," );
	if ( pStr )
	{
		size = atoi( pStr + 1 );
	}


	strcpy( path, "assets" PATH_SEPARATOR "graphics" PATH_SEPARATOR );
	strcat( path, fontName );
	S_strlwr( path );
	gosFont = gos_LoadFont( path );

	if ( gosFont )
		return 0;

	return FONT_NOT_LOADED;

}

long aFont::init( long resourceID )
{
	destroy();
	gosFont = 0;
	gosFont = loadFont( resourceID, size );
	resID = resourceID;

	if ( gosFont )
		return 0;

	return FONT_NOT_LOADED;
}


aFont::~aFont()
{
	destroy();
}



unsigned long aFont::height( ) const
{
	DWORD width, height;
	gos_TextSetAttributes (gosFont, 0, size, false, true, false, false);
	gos_TextStringLength(&width,&height,"ABCDE");
	return height;
}

void	aFont::getSize( DWORD& width, DWORD& height, const char* pText )
{
	gos_TextSetAttributes (gosFont, 0, size, false, true, false, false);
	gos_TextStringLength(&width,&height,pText);

}


unsigned long aFont::height( const char* st, int areaWidth ) const
{
	DWORD width, height;
	gos_TextSetAttributes (gosFont, 0, size, false, true, false, false);
	gos_TextSetRegion( 0, 0, areaWidth, Environment.screenHeight );
	gos_TextStringLength(&width,&height,st);

	DWORD lineCount = 1;

	if ( width > areaWidth - 1 )
	{
		DWORD curLineWidth = 0;

		gosASSERT( strlen( st) < 2048 );

		
		char pLine[2048]; // should be more than adequate

		char* pLastWord = (char*)st;
		char* pTmp = (char*)st;
		char* pTmpLine = (char*)pLine;

		int lineStart = 0;
		int numberOfWordsPerLine = 0;


		bool bHasSpaces = true;
		if (  !strstr( st, " " ) )
		{
			if ( !strstr( st, "\n" ) )
				bHasSpaces = false;
		}

		while( *pTmp != '\0')
		{
			if ( *pTmp == '\n' )
			{
				lineCount++;
				numberOfWordsPerLine = 0;
				curLineWidth = 0;	
				pTmpLine = pLine;
				pLastWord = pTmp+1;
				lineStart = pLastWord - st;
			}
			else if ( !bHasSpaces )
			{
				if ( pTmp > st )
				{
					char tmp = *(pTmp-1);
					if ( !isleadbyte( tmp ) )
					{
						*(pTmpLine) = '\0';
						gos_TextStringLength( &curLineWidth, &height, pLine );
						if ( curLineWidth > areaWidth )
						{
							lineCount++;
							pTmp--;
							lineStart = pTmpLine - pLine;
							pTmpLine = pLine;
							curLineWidth = 0;
							numberOfWordsPerLine = 0;
						}

					}
					
				
				}
			}

			else if ( isspace( *pTmp ) )
			{
				*(pTmpLine) = '\0';
				gos_TextStringLength( &curLineWidth, &height, pLine );
				if ( curLineWidth > areaWidth )
				{
					int last_word_offset = pLastWord - st - lineStart;
					pLine[last_word_offset] = '\0';
					//gos_TextStringLength( &curLineWidth, &height, pLastWord );
					gos_TextStringLength( &curLineWidth, &height, pLine);
					if ( numberOfWordsPerLine == 0 || curLineWidth > areaWidth )
					{
						static bool firstTime = true;
						if (firstTime)
						{
							Assert( true, 0, "this list box item contains a word of greater "
									" area than the list box, giving up" );
							firstTime = false;
						}
						/*There are times when you just can't guarantee that this won't occur,
						so we have to just continue and deal with it.*/
						//return height;

						pLastWord = pTmp;
					} else {
						//SPEW(("DBG", "line: %s\n", pLine));
					}
					lineCount++;
					pTmpLine = pLine;
					pTmp = pLastWord;
					curLineWidth = 0;
					numberOfWordsPerLine = 0;
					lineStart = pLastWord - st;

				}
		
				pLastWord = pTmp;
				numberOfWordsPerLine++;
			}


			*pTmpLine = *pTmp;
			if ( isleadbyte( *pTmpLine ) )
			{
				*(pTmpLine+1) = *(pTmp+1);
			}

			pTmpLine = (char*)_mbsinc( (unsigned char*)pTmpLine );
			pTmp = (char*)_mbsinc( (unsigned char*)pTmp );
		}

		// one last check
		*pTmpLine = '\0';
		gos_TextStringLength( &curLineWidth, &height, pLine );
		if ( curLineWidth > areaWidth )
		{
			lineCount++;
		}

		if ( *pTmp == '\0')
			lineCount++;	
	}

	gos_TextStringLength( &width, &height, "A" );

	return (height) * lineCount ;
}

unsigned long aFont::width( const char* st ) const
{
	DWORD width, height;
	gos_TextSetAttributes (gosFont, 0, size, false, true, false, false);
	gos_TextStringLength(&width,&height,st);
	return width;
}

long aFont::load( const char* fontName )
{
	destroy();

	return init( fontName );
}

void aFont::destroy()
{
	if ( gosFont )
		gos_DeleteFont( gosFont );

	gosFont = 0;
	resID = 0;
	fontName[0] = 0;

}

void aFont::render( const char* text, int xPos, int yPos, int areaWidth, int areaHeight, 
				   DWORD color, bool bBold, int alignment )
{
	gos_TextSetAttributes( gosFont, color, size, true, true, bBold, false, alignment );
	
	if ( areaWidth < 1 )
	{
		if ( alignment == 1 )
		{
			DWORD width, height;
			gos_TextStringLength( &width, &height, text );
			xPos -= width;
			areaWidth = width + 1;
		}
		else
			areaWidth = Environment.screenWidth;
	}

	if ( areaHeight < 1 )
	{
		if ( alignment == 3  ) // bottom
		{
			DWORD width, height;
			gos_TextStringLength( &width, &height, text );
			yPos -= height;
			areaHeight = height + 1;
		}
		
		else
			areaHeight = Environment.screenHeight;
	}

	gos_TextSetRegion( xPos, yPos, xPos + areaWidth, yPos + areaHeight );
	
	gos_TextSetPosition( xPos, yPos );
	gos_TextDraw( text );	

}

HGOSFONT3D aFont::loadFont( long resourceID, long& size )
{
	size = 1;
	char buffer[256];
	cLoadString( resourceID, buffer, 255 );

	char* pStr = strstr( buffer, "," );
	if ( pStr )
	{
		size = -atoi( pStr + 1 );
		*pStr = '\0';
	}

	// MC2_HUD_READABILITY_V1: swap in the larger retail atlas for HUD fonts.
	if ( mc2HudReadabilityV1() )
	{
		const char* repl = mc2HudReadabilityFontOverride( resourceID );
		if ( repl )
			strcpy( buffer, repl );
	}

	char path[256];
	strcpy( path, "assets" PATH_SEPARATOR "graphics" PATH_SEPARATOR );
	strcat( path, buffer );
	S_strlwr( path );
	HGOSFONT3D retFont = gos_LoadFont( path );

	return retFont;
}

aFont::aFont( const aFont& src )
{
	copyData(src);
}

aFont& aFont::operator=( const aFont& src )
{
	copyData(src);
	return *this;
}

void aFont::copyData( const aFont& src )
{
	if ( &src != this )
	{
		if (src.resID )
		{
			init( src.resID );
		}
		else if ( src.fontName[0] != 0 )
		{
			init( src.fontName );
		}

		size = src.size;
	}
}
