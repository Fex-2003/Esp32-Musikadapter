#include "Rules.h"
#include "Config.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <set>
#include <random>
#include <limits>
static std::mt19937 engine(12345);
static uint32_t rng() { return engine(); }
int main() {
  using namespace rules;
  assert(motionFile("Lied1.mp3")); assert(motionFile("Test.MP3"));
  assert(!motionFile("Lied_1.mp3")); assert(!motionFile("Meine Musik.mp3"));
  assert(!motionFile("CON.mp3")); assert(!motionFile("../X.mp3")); assert(!motionFile(".mp3"));
  assert(playlistName("Rock 2026")); assert(playlistName("Mix_(A),1"));
  assert(!playlistName("Rock/Pop")); assert(!playlistName(" Rock")); assert(!playlistName("Rock "));
  assert(!playlistName("CON")); assert(!playlistName("LPT1")); assert(!playlistName(std::string(17,'A')));
  assert(!playlistName(u8"Aerzte \u00e4"));
  assert(musicFile(u8"Sch\u00f6ne Gr\u00fc\u00dfe, Teil; 1.mp3"));
  assert(musicFile(u8"\u30b7 \u8272.mp3")); assert(musicFile(u8"\U0001f3b5.mp3"));
  assert(musicFile("O'Brien & 100% + #1.mp3"));
  assert(!musicFile("../Song.mp3")); assert(!musicFile("Song?.mp3")); assert(!musicFile("Song .mp3"));
  assert(!musicFile("._Song.mp3")); assert(!musicFile("\"Song\".mp3"));
  assert(!musicFile(std::string(157,'a')+".mp3"));
  assert(!utf8(std::string("\xc0\xaf",2))); assert(!utf8(std::string("\xed\xa0\x80",3)));
  assert(!utf8(std::string("\xf4\x90\x80\x80",4))); assert(!utf8(std::string("\xe3\x82",2)));
  assert(!utf8(std::string("A\0B",3))); assert(!utf8("a\nb"));
  size_t units=0; assert(utf8(u8"\u00e4\u30b7\U0001f3b5",&units) && units==4);
  assert(btName("Audio-ESP32-BT")); assert(!btName(std::string(33,'x')));
  double w=0; assert(weight("0.5",w) && w==0.5); assert(weight("1000",w));
  assert(!weight("nan",w)); assert(!weight("1001",w)); assert(!weight("1,5",w));
  assert(!weight("1.2.3",w)); assert(!weight("-1",w)); assert(!weight(".",w));
  uint32_t n; assert(unsignedNumber("600",600,n) && n==600);
  assert(!unsignedNumber("601",600,n)); assert(!unsignedNumber("-1",600,n));
  assert(!unsignedNumber("4294967296",0xffffffffu,n));
  assert(naturalCompare("1 Lied.mp3", "2 Lied.mp3") < 0);
  assert(naturalCompare("2 Lied.mp3", "10 Lied.mp3") < 0);
  assert(naturalCompare("1 Lied.mp3", "01 Lied.mp3") < 0);
  assert(naturalCompare("Titel 12.mp3", "Titel 12.mp3") == 0);
  assert(html("<a & \"b\" 'c'>") == "&lt;a &amp; &quot;b&quot; &#39;c&#39;&gt;");
  assert(url("A B,+&") == "A%20B%2C%2B%26");
  assert(json("A\n\"\\") == "\"A\\u000a\\\"\\\\\"");
  assert(weightedPick({},-1,0)==-1); assert(weightedPick({0,0},-1,0)==-1);
  assert(weightedPick({0,9,0},1,0)==1);
  for(int i=0;i<1000;i++) { assert(weightedPick({0.5,9},0,rng())==1); assert(weightedPick({0.5,9},1,rng())==0); }
  std::vector<double> weights{.5,1.5,1,2};
  int counts[4]={};
  for(int i=0;i<100000;i++) ++counts[weightedPick(weights,-1,rng())];
  const double expect[]={.1,.3,.2,.4};
  for(int i=0;i<4;i++) assert(std::abs(counts[i]/100000.0-expect[i])<.01);
  int last=-1;
  for(int i=0;i<10000;i++) { int next=weightedPick(weights,last,rng()); assert(next!=last); last=next; }
  std::vector<uint16_t> ord;
  for(size_t count=0;count<=512;count++) {
    for(int rep=0;rep<20;rep++) {
      order(ord,count,true,count ? static_cast<int>(count-1) : -1,rng);
      assert(ord.size()==count); assert(std::set<uint16_t>(ord.begin(),ord.end()).size()==count);
      if(count>1) assert(ord.front()!=count-1);
    }
  }
  order(ord,10,false,0,rng); for(int i=0;i<10;i++) assert(ord[i]==i);
  std::cout << "PASS: Namen/UTF-8, Escaping, Zahlen, gewichtete Auswahl, Shuffle-Permutationen\n";
}
