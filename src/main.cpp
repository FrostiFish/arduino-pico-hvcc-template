#include <Arduino.h>
#include <I2S.h>                 // Arduino-Pico I2S library
#include <Adafruit_TinyUSB.h>    // USB MIDI transport (bundled with the core)
#include <MIDI.h>                // FortySevenEffects MIDI Library

#include "Heavy_MidiToSine.hpp"
using HeavyPatch = Heavy_MidiToSine;

// Heavy's internal receiver hash for [notein] ("__hv_notein"),
// the same value hvcc's own wrappers use
#define HV_HASH_NOTEIN 0x67E37CA3

constexpr int PIN_BCLK    = 26;     // LRCLK is automatically BCLK + 1 (27)
constexpr int PIN_DOUT    = 28;
constexpr int SAMPLE_RATE = 48000;
constexpr int BLOCK       = 32;     // frames per Heavy call (multiple of 4/8/16)
constexpr int MAX_OUT     = 8;

// ---- Shared between the cores -------------------------------------------
// Core 1 creates the Heavy context and publishes the pointer once audio is
// running. Core 0 only ever sends messages to it (Heavy's send functions are
// thread-safe), and does nothing while the pointer is still null.
HeavyPatch *volatile heavy = nullptr;

// ---- Core 0: USB MIDI ---------------------------------------------------
Adafruit_USBD_MIDI usb_midi;
MIDI_CREATE_INSTANCE(Adafruit_USBD_MIDI, usb_midi, MIDI);

void onNoteOn(byte channel, byte note, byte velocity) {
  HeavyPatch *h = heavy;
  if (!h) return;
  h->sendMessageToReceiverV(HV_HASH_NOTEIN, 0, "fff",
                            (float)note, (float)velocity,
                            (float)(channel - 1));       // 0-based channel
}

void onNoteOff(byte channel, byte note, byte /*velocity*/) {
  HeavyPatch *h = heavy;
  if (!h) return;
  h->sendMessageToReceiverV(HV_HASH_NOTEIN, 0, "fff",
                            (float)note, 0.0f, (float)(channel - 1));
}

void setup() {
  MIDI.begin(MIDI_CHANNEL_OMNI);
  MIDI.setHandleNoteOn(onNoteOn);    // note-on with velocity 0 is delivered
  MIDI.setHandleNoteOff(onNoteOff);  // as note-off by the library
}

void loop() {
  MIDI.read();                       // fires the handlers above
}

// ---- Core 1: Heavy + I2S ------------------------------------------------
// Keep every I2S call and every process call on this core.
I2S i2s(OUTPUT);
float hvOut[BLOCK * MAX_OUT];        // interleaved Heavy output
int numOut = 0;

// float [-1, 1] -> signed 32-bit. 2147483520 is the largest float below 2^31,
// so full scale can't overflow int32 when the float is rounded.
static inline int32_t toPcm32(float x) {
  if (x > 1.0f)  x = 1.0f;
  if (x < -1.0f) x = -1.0f;
  return (int32_t)(x * 2147483520.0f);
}

void setup1() {
  HeavyPatch *h = new HeavyPatch(SAMPLE_RATE);
  numOut = h->getNumOutputChannels();
  if (numOut < 1 || numOut > MAX_OUT) {
    while (true) delay(1000);        // patch has an unsupported dac~ setup
  }

  i2s.setBCLK(PIN_BCLK);             // call these before begin()
  i2s.setDATA(PIN_DOUT);
  i2s.setBitsPerSample(32);
  if (!i2s.begin(SAMPLE_RATE)) {
    while (true) delay(1000);        // I2S failed to start
  }

  heavy = h;                         // publish only once audio can run
}

void loop1() {
  HeavyPatch *h = heavy;
  if (!h) return;

  h->processInline(nullptr, hvOut, BLOCK);   // layout: [L0..L31, R0..R31]

  for (int i = 0; i < BLOCK; i++) {
    float l = hvOut[i];
    float r = (numOut > 1) ? hvOut[BLOCK + i] : l;   // mono -> both sides
    i2s.write32(toPcm32(l), toPcm32(r));
  }
}