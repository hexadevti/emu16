// main.cpp -- desktop entry point: the Arduino setup()/loop() pair, run forever.
#include "../../../board.h"

#if defined(BOARD_DESKTOP)

void setup();
void loop();

int main(int, char **) {
  setup();
  for (;;) loop();
}

#endif
