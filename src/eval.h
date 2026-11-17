// Hand-crafted evaluation. The neural net learns a correction on top of this.
#pragma once
#include "chess.h"

// Centipawns from the side to move's view. posScale (0..1) dials down positional knowledge for weak bots.
int evalHCE(const Position& pos, float posScale = 1.f);
int pieceValue(int type);
