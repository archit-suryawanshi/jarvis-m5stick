// Jarvis's avatar: a small pixel-art robot that reacts to what Jarvis is doing.
#pragma once

#include <M5Unified.h>

enum class Mood { Idle, Listening, Thinking, Error, Sleepy };

// Size on screen: a 48x56 pixel-art sprite drawn at 2x.
static const int AVATAR_W = 96, AVATAR_H = 112;

// Draws the avatar with its top-left corner at (x, y). `level` (0..1) is the
// microphone level, used while listening. Call once per frame: blinks,
// glances and colour changes are animated from the clock.
void drawAvatar(M5Canvas &canvas, int x, int y, Mood mood, float level = 0);
