// Shared surface of the logisticsmissioninfo.cpp translation units (logisticsmissioninfo.cpp, logisticsmissioninfo_{progress,query}.cpp).
//
// logisticsmissioninfo.cpp keeps construction, the campaign file parse, load and save; the mission progression and the
// per-mission accessors are split out (see DOC in each piece).
#ifndef LOGISTICSMISSIONINFO_INTERNAL_H
#define LOGISTICSMISSIONINFO_INTERNAL_H

#include"mclib.h"

#include"logisticsmissioninfo.h"

#ifndef FITINIFILE_H
#include"inifile.h"
#endif

#include"logisticserrors.h"
#include"multplyr.h"

// Globals defined in logisticsmissioninfo.cpp (or elsewhere in the engine) that the split pieces share.
extern char missionName[1024];

#endif // LOGISTICSMISSIONINFO_INTERNAL_H
