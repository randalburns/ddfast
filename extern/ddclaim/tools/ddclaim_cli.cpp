// The ddclaim command: solves endings given as text.
//
// usage: ddclaim [file]                      one position per line, from the file or standard input
//        ddclaim <trump> <leader> <north> <east> <south> <west>
//
// A position is six words: the trump suit (S H D C, or N for no trumps), the hand on lead
// (N E S W), then the four hands in the order North East South West. A hand lists its
// spades, hearts, diamonds and clubs separated by dots, each suit as ranks from AKQJT98765432
// (an empty suit is empty, or "-"): "AK.Q..T9" is the ace and king of spades, the queen of
// hearts, no diamonds and the ten and nine of clubs. Every hand must hold the same number of
// cards, 1 to 7. Blank lines and lines starting with '#' are skipped.
//
// Output: one line per position, "<North-South tricks> <East-West tricks>". All the
// positions are solved as one batch.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "ddclaim.hpp"

namespace {

bool parse_hand(const std::string& text, uint16_t hand[4]) {
  static const char ranks[] = "23456789TJQKA";
  int suit = 0;
  hand[0] = hand[1] = hand[2] = hand[3] = 0;
  for (char ch : text) {
    if (ch == '.') {
      if (++suit > 3) return false;
    } else if (ch != '-') {
      const char up = static_cast<char>(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
      const char* at = up ? std::strchr(ranks, up) : nullptr;
      if (!at) return false;
      const uint16_t bit = static_cast<uint16_t>(1u << (at - ranks));
      if (hand[suit] & bit) return false;
      hand[suit] |= bit;
    }
  }
  return suit == 3;
}

// Parses one position; on failure says why in `error`.
bool parse_position(const std::vector<std::string>& w, ddclaim::Position* p, std::string* error) {
  if (w.size() != 6) {
    *error = "expected 6 words: trump, leader and four hands";
    return false;
  }
  static const char trumps[] = "SHDCN", seats[] = "NESW";
  const char t = static_cast<char>(w[0].size() == 1 ? (w[0][0] & ~0x20) : 0);
  const char l = static_cast<char>(w[1].size() == 1 ? (w[1][0] & ~0x20) : 0);
  if (!t || !std::strchr(trumps, t)) {
    *error = "the trump suit must be S, H, D, C or N";
    return false;
  }
  if (!l || !std::strchr(seats, l)) {
    *error = "the leader must be N, E, S or W";
    return false;
  }
  p->trump = static_cast<uint8_t>(std::strchr(trumps, t) - trumps);
  p->leader = static_cast<uint8_t>(std::strchr(seats, l) - seats);
  for (int q = 0; q < 4; q++)
    if (!parse_hand(w[2 + q], p->hand[q])) {
      *error = "cannot read the hand \"" + w[2 + q] + "\" (four suits separated by dots, ranks AKQJT98765432)";
      return false;
    }
  if (ddclaim::cards_per_hand(*p) < 0) {
    *error = "the hands must hold the same number of cards, 1 to 7, with no card twice";
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<ddclaim::Position> positions;
  std::string error;
  if (argc == 7) {
    ddclaim::Position p;
    if (!parse_position(std::vector<std::string>(argv + 1, argv + 7), &p, &error)) {
      fprintf(stderr, "ddclaim: %s\n", error.c_str());
      return 2;
    }
    positions.push_back(p);
  } else if (argc <= 2 && !(argc == 2 && (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help"))) {
    std::ifstream file;
    if (argc == 2) {
      file.open(argv[1]);
      if (!file) {
        fprintf(stderr, "ddclaim: cannot open %s\n", argv[1]);
        return 2;
      }
    }
    std::istream& in = argc == 2 ? static_cast<std::istream&>(file) : std::cin;
    std::string line;
    for (int number = 1; std::getline(in, line); number++) {
      std::istringstream words(line);
      std::vector<std::string> w;
      for (std::string word; words >> word;) w.push_back(word);
      if (w.empty() || w[0][0] == '#') continue;
      ddclaim::Position p;
      if (!parse_position(w, &p, &error)) {
        fprintf(stderr, "ddclaim: line %d: %s\n", number, error.c_str());
        return 2;
      }
      positions.push_back(p);
    }
  } else {
    fprintf(stderr,
            "usage: ddclaim [file]\n"
            "       ddclaim <trump> <leader> <north> <east> <south> <west>\n"
            "  trump S H D C or N; leader N E S W; a hand is spades.hearts.diamonds.clubs, e.g. AK.Q..T9\n"
            "  prints North-South's and East-West's tricks for each position\n");
    return 2;
  }
  if (positions.empty()) return 0;
  ddclaim::init();
  std::vector<uint8_t> tricks(positions.size());
  ddclaim::solve(positions.data(), positions.size(), tricks.data());
  for (size_t i = 0; i < positions.size(); i++) {
    const int cards = ddclaim::cards_per_hand(positions[i]);
    printf("%d %d\n", tricks[i], cards - tricks[i]);
  }
  return 0;
}
