/*
 * arduino_glue.cpp - Arduino framework entry points.
 * Bridges Arduino setup()/loop() to bs_init()/bs_run().
 */
#include <Arduino.h>

extern "C" {
    void bs_init(void);
    void bs_run(void);
}

void setup() { bs_init(); }
void loop()  { bs_run();  }
