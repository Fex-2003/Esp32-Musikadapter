#include "PlaylistOrder.h"
#include <cassert>
#include <iostream>
using playlist_order::takeRequested;
int main() {
  std::vector<uint16_t> order{0,1,2,3,4};
  size_t pos=1; // Titel 0 laeuft, Rest ist noch ausstehend.
  assert(takeRequested(order,pos,3));
  assert((order==std::vector<uint16_t>{0,3,1,2,4}) && pos==2);
  assert(!takeRequested(order,pos,0)); // bereits gespielt: extra Wunsch
  assert(pos==2 && order[pos]==1);
  assert(takeRequested(order,pos,4));
  assert((order==std::vector<uint16_t>{0,3,4,1,2}) && pos==3);
  assert(takeRequested(order,pos,1) && pos==4);
  assert(takeRequested(order,pos,2) && pos==5);
  assert(!takeRequested(order,pos,2) && pos==5); // Wunsch nach Durchlaufende
  pos=9;
  assert(!takeRequested(order,pos,1) && pos==9);
  // Dieselbe Operation behaelt die vorhandene Shuffle-Permutation bei.
  order={4,2,0,3,1}; pos=1;
  assert(takeRequested(order,pos,3));
  assert((order==std::vector<uint16_t>{4,3,2,0,1}) && pos==2);
  std::cout<<"Playlist-Wunsch und Reihenfolge bestanden\n";
}
