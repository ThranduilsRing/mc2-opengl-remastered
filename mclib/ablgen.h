//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
//***************************************************************************
//
//								GENERAL.H
//
//***************************************************************************

#ifndef ABLGEN_H
#define	ABLGEN_H

//***************************************************************************

//---------------------------------------------------------------------
// Set this, once we have the actual executor modules up and running...
inline constexpr int USE_COMPOUND_STATEMENTS = 0;
#define	ANALYZE_ON					0
inline constexpr int ORDERS_ON = 1;

inline constexpr char CHAR_FORMFEED = '\f';
inline constexpr char CHAR_EOF = '\x7f';

inline constexpr int MAX_INCLUDE_DEPTH = 6;
inline constexpr int MAXLEN_FILENAME = 256;
inline constexpr int MAXLEN_SOURCELINE = 2048;
inline constexpr int MAXLEN_PRINTLINE = 80;
#define	MAXLEN_TOKENSTRING			MAXLEN_SOURCELINE
inline constexpr int MAX_LINES_PER_PAGE = 50;
inline constexpr int MAXLEN_STRING_CONSTANT = 1024;
inline constexpr int MAX_NESTING_LEVEL = 3;

inline constexpr int LEN_DATESTRING = 26;

inline constexpr int MAXSIZE_CODE_BUFFER = 20480;

inline constexpr int MAXSIZE_STACK = 10240;
inline constexpr int STACK_FRAME_HEADER_SIZE = 4;

inline constexpr int SYSTEM_HEAP_SIZE = 1024 * 1024; // General system memory...

// Masher Commands available...
inline constexpr int COMMAND_HELP_SHORT = 0;
inline constexpr int COMMAND_WAIT_LONG = 1;
inline constexpr int COMMAND_WAIT_SHORT = 2;
inline constexpr int COMMAND_RANDOM_LONG = 3;
inline constexpr int COMMAND_RANDOM_SHORT = 4;
inline constexpr int COMMAND_NUM_RUNS_LONG = 5;
inline constexpr int COMMAND_NUM_RUNS_SHORT = 6;

inline constexpr int ABL_NO_ERR = 0;

inline constexpr int MAX_ORDERS = 65535;

//***************************************************************************

using Address = char*;

//***************************************************************************

#endif

