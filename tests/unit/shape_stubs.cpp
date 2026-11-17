#include "tgl.h"

int g_shapeDestroys = 0;
void TG_Shape::destroy (void) { ++g_shapeDestroys; }
