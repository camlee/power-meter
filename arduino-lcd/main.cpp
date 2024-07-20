#include <Arduino.h>

#include "buttons.h"
#include "display.h"
#include "sensor.h"
#include "store.h"
#include "ui.h"


Display display = Display();
ButtonManager buttonManager = ButtonManager();
SensorManager sensorManager = SensorManager();
Store store = Store();
UI ui = UI(&display, &sensorManager, &store);

void setup(){
    Serial.begin(9600);

    store.setup();
    display.setup();
    buttonManager.setup();
    sensorManager.setup(
        store.getSavedEnergyPanel(),
        store.getSavedEnergyLoad()
        );
    ui.setup();

}

void loop(){
    int button;

    button = buttonManager.popPressedButton();
    ui.handleButton(button);
    display.refresh();
    buttonManager.refresh();
    sensorManager.refresh();
    store.refresh(
        sensorManager.getEnergy(PANEL),
        sensorManager.getEnergy(LOAD)
        );
    ui.refresh();
}

int main(void){
    init();
    setup();
    for (;;){
       loop();
    }
}
