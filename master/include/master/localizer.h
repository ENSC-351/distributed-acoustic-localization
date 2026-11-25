// localizer.h
// Localizer module: takes events out of the event buffer and works out where
// the sound came from.
//
// PLACEHOLDER: the localization isn't written yet. fakeLocalizer.c
// implements this by printing each event, so the networking can be tested
// on its own. The localization only gets events through
// master/eventBuffer.h, so swapping in the real one shouldn't change
// netServer.c.

#ifndef LOCALIZER_H
#define LOCALIZER_H

void Localizer_init(void);
void Localizer_cleanup(void);

#endif
