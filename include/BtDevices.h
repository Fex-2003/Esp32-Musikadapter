#pragma once
#include <Arduino.h>
namespace btdevices {
constexpr size_t MAX_DEVICES=16;
struct Device { uint8_t mac[6]={}; char name[65]={}; bool pending=false; };
void begin();
size_t count();
const Device& at(size_t index);
String format(const uint8_t* mac);
bool parse(const String& text,uint8_t* mac);
bool equal(const uint8_t* a,const uint8_t* b);
bool preferred(uint8_t* mac);
void confirm(const uint8_t* mac);
bool remember(const uint8_t* mac,const char* name);
bool remove(const String& mac,String& message); // persistenter Auftrag
bool removeAll(String& message);
bool pending(const uint8_t* mac);
bool pendingAll();
void reconcile(const uint8_t (*bonds)[6],size_t count); // nur BT-Hauptschleife, nach Stack-Abgleich
void save();
}
