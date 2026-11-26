//-------------------------------------------------------------------------------------------------
// EditorRecent.cpp -- see EditorRecent.h.
//-------------------------------------------------------------------------------------------------
#include "stdafx.h"
#include "EditorRecent.h"
#include "resource.h"
#include "newsinglemission.h"

#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

namespace
{
	std::string recentFile() { return EditorInstallPath("editor_recent.txt"); }
	std::vector<std::string> s_list;
	bool s_loaded = false;

	void load()
	{
		s_loaded = true;
		s_list.clear();
		FILE* f = fopen( recentFile().c_str(), "r" );
		if ( !f ) return;
		char line[1024];
		while ( fgets( line, sizeof( line ), f ) )
		{
			size_t n = strlen( line );
			while ( n > 0 && ( line[n-1] == '\n' || line[n-1] == '\r' ) )
				line[--n] = '\0';
			if ( n > 0 && (int)s_list.size() < EditorRecent::kMax )
				s_list.push_back( line );
		}
		fclose( f );
	}

	void save()
	{
		FILE* f = fopen( recentFile().c_str(), "w" );
		if ( !f ) return;
		for ( size_t i = 0; i < s_list.size(); ++i )
			fprintf( f, "%s\n", s_list[i].c_str() );
		fclose( f );
	}
}

namespace EditorRecent
{

void Push( const char* path )
{
	if ( !path || !path[0] ) return;
	if ( !s_loaded ) load();

	// Dedupe (case-insensitive on Windows paths), move/insert at front.
	for ( size_t i = 0; i < s_list.size(); ++i )
		if ( _stricmp( s_list[i].c_str(), path ) == 0 )
		{
			s_list.erase( s_list.begin() + i );
			break;
		}
	s_list.insert( s_list.begin(), path );
	if ( (int)s_list.size() > kMax )
		s_list.resize( kMax );
	save();
}

int Count()
{
	if ( !s_loaded ) load();
	return (int)s_list.size();
}

const char* Path( int index )
{
	if ( !s_loaded ) load();
	if ( index < 0 || index >= (int)s_list.size() ) return "";
	return s_list[index].c_str();
}

}
