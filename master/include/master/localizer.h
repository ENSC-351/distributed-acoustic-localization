// localizer.h
// Localizer module: a thread takes events out of the event buffer and hands
// them to fusion, which works out where the sound came from.
//
// It only gets events through master/eventBuffer.h, so it never touches
// netServer.c. Until fusion is merged in, it prints each event instead.

#ifndef LOCALIZER_H
#define LOCALIZER_H

void Localizer_init(void);
void Localizer_cleanup(void);

#endif
