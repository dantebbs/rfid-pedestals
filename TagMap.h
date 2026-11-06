// Maps "tag N arrived/left at reader R" to one keyboard character.
//
// Rows are readers, columns are tags: each of the five tags owns two columns,
// the first for "presented" (+) and the second for "removed" (-).
//
//            tag1 + -   tag2 + -   tag3 + -   tag4 + -   tag5 + -
//   reader 1    q w        e r        t y        u i        o p
//   reader 2    a s        d f        g h        j k        l ;
//   reader 3    z x        c v        b n        m ,        . /
//   reader 4    Q W        E R        T Y        U I        O P
//   reader 5    A S        D F        G H        J K        L :
//
// A tag the map hasn't seen takes the next free tag column the first time it
// is presented. The tag IDs and the key tag are kept in flash, separately
// from the settings. Presenting the key tag twice within 5 seconds empties
// the map so five new tags can be learned.
#pragma once
#include <Arduino.h>
#include "Config.h"

static const uint8_t MAP_TAGS = 5;

void tagMapBegin();
// Handles one tag event. Returns the character to send, or 0 for none
// (key tag, map full).
char tagMapEvent(bool appeared, uint8_t reader, const char* uidHex);
void tagMapClear();       // forgets the five tags (the key tag stays)
void tagMapLearnKey();    // the next tag presented at any reader becomes the key tag
void tagMapForgetKey();
void tagMapPrint(Print& out);
