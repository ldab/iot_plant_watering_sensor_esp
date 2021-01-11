
#include "Arduino.h"

/*
typedef enum {
    NOTE_C, NOTE_Cs, NOTE_D, NOTE_Eb, NOTE_E, NOTE_F, NOTE_Fs, NOTE_G, NOTE_Gs,
NOTE_A, NOTE_Bb, NOTE_B, NOTE_MAX } note_t;
*/

// https://en.wikipedia.org/wiki/Ring_Tone_Transfer_Language
// each string contains a duration, pitch, octave and optional dotting (which
// increases the duration of the note by one half)

// Ringtones: https://www.convertyourtone.com/ringtones.html

const char *Muppets[] = {
    "d=4", "o=5", "b=250", "c6",  "c6", "a",  "b",  "8a", "b",  "g",  "p",
    "c6",  "c6",  "a",     "8b",  "8a", "8p", "g.", "p",  "e",  "e",  "g",
    "f",   "8e",  "f",     "8c6", "8c", "8d", "e",  "8e", "8e", "8p", "8e",
    "g",   "2p",  "c6",    "c6",  "a",  "b",  "8a", "b",  "g",  "p",  "c6",
    "c6",  "a",   "8b",    "a",   "g.", "p",  "e",  "e",  "g",  "f",  "8e",
    "f",   "8c6", "8c",    "8d",  "e",  "8e", "d",  "8d", "c"};

const char *HauntHouse[] = {
    "d=4",  "o=5", "b=108", "2a4", "2e",  "2d#", "2b4", "2a4", "2c",  "2d",
    "2a#4", "2e.", "e",     "1f4", "1a4", "1d#", "2e.", "d",   "2c.", "b4",
    "1a4",  "1p",  "2a4",   "2e",  "2d#", "2b4", "2a4", "2c",  "2d",  "2a#4",
    "2e.",  "e",   "1f4",   "1a4", "1d#", "2e.", "d",   "2c.", "b4",  "1a4"};

// https://www.vex.net/~lawrence/ringtones.html#nonmusic

const char *Skala[] = {"d=4", "o=5", "b=160", "32c", "32d",  "32e",
                       "32f", "32g", "32a",   "32b", "32c6", "32b",
                       "32a", "32g", "32f",   "32e", "32d",  "32c"};

const char *Urgent[] = {"d=8", "o=6", "b=500", "c", "e",  "d7", "c", "e",
                        "a#",  "c",   "e",     "a", "c",  "e",  "g", "c",
                        "e",   "a",   "c",     "e", "a#", "c",  "e", "d7"};

note_t noteLookUp(const char *note);

void play(uint8_t channel, const char *music[]) {
  const char numbers[] = "1234567890";

  uint8_t duration = (uint8_t)music[0][2];  // how long or short a note lasts
  uint8_t octave =
      (uint8_t)music[1][2];  // the interval between one musical pitch and
                             // another with double its frequency

  char _tempo[4];
  strcpy(_tempo, music[2] + 2);
  uint8_t tempo =
      atoi(_tempo);  // usually measured in beats per minute (or bpm)

  uint8_t noteDuration = (60000 * 2) / tempo;

  note_t thisNote;
  uint8_t thisDuration;
  uint8_t thisOctave;
  // TODO, sizeof is funny
  for (int i = 3; i < ((sizeof(music) / sizeof(music[0])) - 3); i++) {
    uint8_t _d = strspn(music[i], numbers);

    if (_d > 0) {
      thisDuration = noteDuration / atoi(music[i]);
    } else {
      thisDuration = noteDuration / duration;
    }

    uint8_t _n = strcspn(music[i] + _d, numbers);
    char note_char[4] = "";
    strncpy(note_char, music[i] + _d, _n);
    thisNote = noteLookUp(note_char);

    uint8_t _o = strspn(music[i] + _d + _n, numbers);

    if (_o > 0) {
      thisOctave = atoi(music[i + _d + _n]);
    } else {
      thisOctave = octave;
    }

    if (strchr(music[i], '.') != NULL) {
      thisDuration *= 1.5;
    }

    if (strcmp(note_char, "p") == 0) {
      ledcWrite(channel, 0);
      delay(thisDuration);
    } else {
      ledcWriteNote(channel, thisNote, thisOctave);
      delay(thisDuration);
      ledcWrite(channel, 0);
    }
  }
}

note_t noteLookUp(const char *note) {
  if (strcmp(note, "a") == 0) {
    return NOTE_A;
  }
  if (strcmp(note, "a#") == 0) {
    return NOTE_Bb;
  }
  if (strcmp(note, "b") == 0) {
    return NOTE_B;
  }
  if (strcmp(note, "c") == 0) {
    return NOTE_C;
  }
  if (strcmp(note, "c#") == 0) {
    return NOTE_Cs;
  }
  if (strcmp(note, "d") == 0) {
    return NOTE_D;
  }
  if (strcmp(note, "d#") == 0) {
    return NOTE_Eb;
  }
  if (strcmp(note, "e") == 0) {
    return NOTE_E;
  }
  if (strcmp(note, "f") == 0) {
    return NOTE_F;
  }
  if (strcmp(note, "f#") == 0) {
    return NOTE_Fs;
  }
  if (strcmp(note, "g") == 0) {
    return NOTE_G;
  }
  if (strcmp(note, "g#") == 0) {
    return NOTE_Gs;
  }
  Serial.printf("Note: %s not identified\n", note);
  return NOTE_MAX;
}