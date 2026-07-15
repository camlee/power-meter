#include <Arduino.h>

#include "buttons.h"
#include "display.h"
#include "sensor.h"
#include "store.h"
#include "telemetry.h"
#include "ui.h"


Display display = Display();
ButtonManager buttonManager = ButtonManager();
Store store = Store();
SensorManager sensorManager = SensorManager(&store);
TelemetryTransmitter telemetry = TelemetryTransmitter(&sensorManager);
UI ui = UI(&display, &sensorManager, &store);

void setup(){
    Serial.begin(9600);

    store.setup();
    display.setup();
    buttonManager.setup();
    sensorManager.setup();
    telemetry.setup();
    ui.setup();

}

void loop(){
    int button;

    button = buttonManager.popPressedButton();
    ui.handleButton(button);
    display.refresh();
    buttonManager.refresh();
    sensorManager.refresh();
    telemetry.refresh();
    store.refresh();
    ui.refresh();
}
