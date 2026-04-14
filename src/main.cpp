#include <Arduino.h>

#include <Wire.h>
#include <LiquidCrystal_I2C.h>

#include <Button.h>
#include <LcdMenu.h>
#include <MenuScreen.h>

#include <display/LiquidCrystal_I2CAdapter.h>
#include <input/ButtonAdapter.h>
#include <renderer/CharacterDisplayRenderer.h>
#include <ItemRange.h>
#include <ItemCommand.h>
#include <ItemBack.h>
#include <ItemBool.h>
#include <ItemSubMenu.h>

#define LCD_ROWS 4
#define LCD_COLS 20

float safteyVoltage = 10.5;

// ================= LCD =================
LiquidCrystal_I2C lcd(0x27, LCD_COLS, LCD_ROWS);
LiquidCrystal_I2CAdapter lcdAdapter(&lcd);
CharacterDisplayRenderer renderer(&lcdAdapter, LCD_COLS, LCD_ROWS, 0x7E, 0);
LcdMenu menu(renderer);

extern MenuScreen* mainScreen;  

// ================= BUTTONS =================
Button upBtn(16);
Button downBtn(17);
Button enterBtn(5);

// no repeat -> prevents double increments
ButtonAdapter upBtnA(&menu, &upBtn, UP);
ButtonAdapter downBtnA(&menu, &downBtn, DOWN);
ButtonAdapter enterBtnA(&menu, &enterBtn, ENTER);

// ================= BATTERY DATA =================
int selectedBattery = 1;

float battV[4];
bool battConnected[4];

// ================= PARAMETERS =================
int quickDischargeTime = 10;
int quickCycles = 3;

float fullCutoffVoltage = 10.5;
int fullCycles = 3;

// ================= SETTINGS =================
bool backlightOn = true;

// ================= HARDWARE =================
const int dischargePins[4] = {12,13,14,27};
const int batteryPins[4]   = {33,32,35,34};
const int dischargeSensePin = 26;

#define ADC_MAX 4095.0
#define VREF 3.3
#define DIV_RATIO 0.2418

#define CONNECTED_MIN 9.0
#define FULLY_CHARGED 12.7
#define DISCHARGER_EMPTY 10.5

// ================= STATE =================
enum CycleMode {
    MODE_IDLE,
    MODE_QUICK,
    MODE_FULL,
    MODE_CUSTOM
};

float current = 0;

CycleMode mode = MODE_IDLE;

int activeBattery = -1;
int cycleCount = 0;
bool discharging = false;

unsigned long dischargeStart = 0;
unsigned long lastSwitch = 0;

bool runScreen = false;
char runStatus[21] = "Idle";

// ================= COMMANDS =================
void startQuickCycle()
{
    cycleCount = 0;
    mode = MODE_QUICK;
    runScreen = true;
}

void startFullDischarge()
{
    cycleCount = 0;
    mode = MODE_FULL;
    runScreen = true;
}

void startDischarge()
{
    cycleCount = 0;
    mode = MODE_CUSTOM;
    runScreen = true;
}

void cancelCycle()
{
    //disableAll();

    mode = MODE_IDLE;
    runScreen = false;
}

// ================= MENUS =================
MENU_SCREEN(quickCycleSubMenu, quickItems,
    ITEM_RANGE<int>("Time (min)", quickDischargeTime, 1, 1, 90, [](int v){ quickDischargeTime = v; }, "%d"),
    ITEM_RANGE<int>("Cycles", quickCycles, 1, 1, 10, [](int v){ quickCycles = v; }, "%d"),
    ITEM_COMMAND("Start", startQuickCycle),
    ITEM_BACK("Back")
);

MENU_SCREEN(fullDischargeSubMenu, fullItems,
    ITEM_RANGE<float>("Cutoff V", fullCutoffVoltage, 0.1, 10.5, 12, [](float v){ fullCutoffVoltage = v; }, "%.1f"),
    ITEM_RANGE<int>("Cycles", fullCycles, 1, 1, 10, [](int v){ fullCycles = v; }, "%d"),
    ITEM_COMMAND("Start", startFullDischarge),
    ITEM_BACK("Back")
);

MENU_SCREEN(customSubMenu, customItems,
    ITEM_RANGE<int>("Battery", selectedBattery, 1, 1, 4, [](int v){ selectedBattery = v; }, "B%d"),
    ITEM_COMMAND("Start Discharge", startDischarge),
    ITEM_BACK("Back")
);

MENU_SCREEN(settingsSubMenu, settingsItems,
    ITEM_BOOL("Backlight", backlightOn, "On", "Off", [](bool v){
        backlightOn = v;
        lcdAdapter.setBacklight(v);
    }),
    ITEM_BACK("Back")
);

MENU_SCREEN(mainScreen, mainItems,
    ITEM_SUBMENU("Quick Cycle", quickCycleSubMenu),
    ITEM_SUBMENU("Full Discharge", fullDischargeSubMenu),
    ITEM_SUBMENU("Custom", customSubMenu),
    ITEM_SUBMENU("Settings", settingsSubMenu)
);

// ================= VOLTAGE =================
#define SAMPLE_INTERVAL_MS 100      // sample every...
#define AVERAGE_TIME_MS    1000  
#define MAX_SAMPLES (AVERAGE_TIME_MS / SAMPLE_INTERVAL_MS)

float readVoltage(int pin)
{
    static float buffer[MAX_SAMPLES];
    static int index = 0;
    static int count = 0;
    static float sum = 0;
    static unsigned long lastSample = 0;

    unsigned long now = millis();

    // take a new sample when it's time (non-blocking)
    if (now - lastSample >= SAMPLE_INTERVAL_MS)
    {
        lastSample = now;

        int raw = analogRead(pin);

        // remove oldest sample from sum
        sum -= buffer[index];

        // store new sample
        buffer[index] = raw;

        // add new sample to sum
        sum += raw;

        index++;
        if (index >= MAX_SAMPLES) index = 0;

        if (count < MAX_SAMPLES) count++;
    }

    // compute rolling average
    float avgRaw = sum / count;
    float v = (avgRaw / ADC_MAX) * VREF;
    float measured = v / DIV_RATIO;

    return 0.7006f * measured + 3.214f; //idk it was inaccurate
}
// ================= BATTERY DETECT =================
void updateBatteryDetect()
{
    for(int i=0;i<4;i++)
    {
        battV[i] = readVoltage(batteryPins[i]);
        battConnected[i] = battV[i] > CONNECTED_MIN;
    }
}

// ================= START DISCHARGE =================
void startDischargeBattery(int bat)
{
    for(int i=0;i<4;i++)
        digitalWrite(dischargePins[i], LOW);

    delay(30);

    digitalWrite(dischargePins[bat], HIGH);

    activeBattery = bat;
    discharging = true;
    dischargeStart = millis();

    sprintf(runStatus,"Discharging B%d",bat+1);
}

// ================= FIND NEXT READY =================
int findNextReady()
{
    for(int i=0;i<4;i++)
    {
        if(!battConnected[i]) continue;

        if(battV[i] >= FULLY_CHARGED)
            return i;
    }

    return -1;
}

// ================= CYCLER =================
void updateCycler()
{
    if(mode == MODE_IDLE) return;

    updateBatteryDetect();

    if(discharging)
    {
        float v = battV[activeBattery];

        if(mode == MODE_QUICK)
        {
            if(millis() - dischargeStart >
               (unsigned long)quickDischargeTime * 60000UL)
            {
                digitalWrite(dischargePins[activeBattery], LOW);
                discharging = false;
                lastSwitch = millis();
                cycleCount++;
            }
        }

        if(mode == MODE_FULL || mode == MODE_CUSTOM)
        {
            if(v <= fullCutoffVoltage)
            {
                digitalWrite(dischargePins[activeBattery], LOW);
                discharging = false;
                lastSwitch = millis();
                cycleCount++;
            }
        }

        if (v <= safteyVoltage-0.1)
        {
          digitalWrite(dischargePins[activeBattery], LOW);
          //ERROR TELL USER TO DISCONNECT BATTERIES
        }

        return;
    }

    float disV = readVoltage(dischargeSensePin);

    if(disV > DISCHARGER_EMPTY)
    {
        strcpy(runStatus,"Relay Fail?");//error state instruct user to imediately disconnect batteries with buzzer alarm
        return;
    }

    if(millis() - lastSwitch < 300)
        return;

    if(mode == MODE_QUICK && cycleCount >= quickCycles)
    {
        strcpy(runStatus,"Finished");
        cancelCycle();
        return;
    }

    if(mode == MODE_FULL && cycleCount >= fullCycles)
    {
        strcpy(runStatus,"Finished");
        cancelCycle();
        return;
    }

    if(mode == MODE_CUSTOM)
    {
        int b = selectedBattery - 1;

        if(battConnected[b])
            startDischargeBattery(b);
        else
            sprintf(runStatus,"Battery %d missing",selectedBattery);

        return;
    }

    int next = findNextReady();

    if(next != -1)
    {
        startDischargeBattery(next);
    }
    else
    {
        strcpy(runStatus,"Wait Charge");
    }
}

// ================= RUN SCREEN =================
void drawRunScreen()
{
    //Row 0: whats it doing
    lcd.setCursor(0,0);
    if(mode == MODE_QUICK)
    {
      lcd.print("Quick Cycle  ");
    }
    else if(mode == MODE_FULL)
    {
      lcd.print("Full Cycle   ");
    }
    else if(mode == MODE_CUSTOM)
    {
      lcd.print("Custom Cycle ");
    }
    
    // Row 1: Current action / active battery
    lcd.setCursor(0,1);
    if(discharging && activeBattery >= 0)
        lcd.printf("DISCHARE B%d", activeBattery+1, " ");
    else if(mode != MODE_IDLE)
        lcd.print("Wait charge  ");
    else
        lcd.print("Unknown      ");

    for(int i=0;i<4;i++)
    {
        lcd.setCursor(13,i); // i+1 because row 0 is status
        lcd.print(i+1);
        lcd.print("|");

        if(battConnected[i])
        {
            lcd.print(battV[i],2); // print voltage with 2 decimals
        }
        else
        {
            lcd.print("-----");     // disconnected battery
        }
    }

    //row 3
    lcd.setCursor(0,2);
    lcd.print("             ");

    //row 4 Cancel
    lcd.setCursor(0,3);
    lcd.print("->Cancel     ");
}

// ================= SETUP =================
void setup()
{
    Wire.begin(21,22);

    upBtn.begin();
    downBtn.begin();
    enterBtn.begin();

    renderer.begin();
    Serial.begin(9600);

    menu.setScreen(mainScreen);

    for(int i=0;i<4;i++)
    {
        pinMode(dischargePins[i], OUTPUT);
        digitalWrite(dischargePins[i], LOW);
    }

    analogReadResolution(12);
}

// ================= LOOP =================
void loop()
{
    if(runScreen)
    {
        updateCycler();
        drawRunScreen();

        if(enterBtn.pressed())
            cancelCycle();

        return;
    }

    upBtnA.observe();
    downBtnA.observe();
    enterBtnA.observe();
}

void disableAll()
{
    // Turn off all discharge pins
    for(int i = 0; i < 4; i++)
    {
        digitalWrite(dischargePins[i], LOW);
    }

    discharging = false;
    activeBattery = -1;
}