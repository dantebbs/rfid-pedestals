// Turns repeated sightings into "appeared" and "disappeared" events.
//
// Each reader tracks its own tags: a tag appears when a reader first sees it
// and disappears once that reader hasn't seen it for removeMs. Events carry
// the reader number, so moving a tag from R1 to R2 gives "1-UID" then "2+UID".
#pragma once
#include <Arduino.h>
#include "Config.h"

typedef void (*TagEventFn)(bool appeared, uint8_t reader, const char* uidHex);

void trackerBegin(TagEventFn onEvent);
void trackerSeen(uint8_t reader, const uint8_t* uid, uint8_t len);
void trackerService(uint32_t nowMs);
void trackerClear();  // reports every present tag as gone, e.g. on a mode change
void trackerPrint(Print& out);
