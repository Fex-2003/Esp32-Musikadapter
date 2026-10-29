#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace playlist_order {
// Einen noch ausstehenden Titel an die aktuelle Position verschieben und
// verbrauchen. Bereits gespielte Titel bleiben zusaetzliche Wunsch-Titel.
inline bool takeRequested(std::vector<uint16_t>& order, size_t& position, uint16_t index) {
  if(position>order.size()) return false;
  auto first=order.begin()+position;
  auto found=std::find(first,order.end(),index);
  if(found==order.end()) return false;
  std::rotate(first,found,found+1);
  ++position;
  return true;
}
}
