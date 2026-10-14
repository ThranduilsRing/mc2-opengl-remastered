#pragma once

#include <string>

class CObjectiveCondition;
class CObjectiveAction;

// Lists and detail viewers share these sentences so their labels stay consistent.
std::string objReadableCondition(CObjectiveCondition* cond);
std::string objReadableAction(CObjectiveAction* act);
