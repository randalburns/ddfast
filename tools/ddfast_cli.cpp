// ddfast: solves bridge deals from the command line.
//
// usage: ddfast [options] [file ...]        (no file: standard input)
//   -t, --threads N   solve on N worker threads (default 1; 0: every available CPU)
//   -d, --dir DIR     the table directory (default $DDFAST_TABLES, else build/tables)
//   -s, --strain X    default strain for lines that give none: S H D C or N
//   -l, --declarer X  default declarer for lines that give none: N E S W
//   --cpu             prints the code paths this build uses and the CPU's relevant features,
//                     then exits (0: the CPU runs this build; a CPU that lacks something the
//                     build uses stops every ddfast program at load with a message, status 1)
//
// Input: one deal per line, in PBN notation, optionally inside a PBN tag:
//   N:AKQ2.J93.T84.Q76 J96.A84.AQ752.T3 T8754.KT.K9.K942 3.Q7652.J63.AJ85
//   [Deal "N:AKQ2.J93.T84.Q76 J96.A84.AQ752.T3 T8754.KT.K9.K942 3.Q7652.J63.AJ85"]
// The first letter is the seat of the first hand; hands follow clockwise, suits in the order
// S.H.D.C, ranks AKQJT98765432 (10 also accepted). A line may end with a strain and a
// declarer ("... N S": no trumps, South declares); then the output is declarer's tricks.
// Otherwise (and with no defaults) the output is the full table, one line per deal:
//   N:... | S 8 5 8 5 | H 7 6 7 6 | D 9 4 9 4 | C 6 7 6 7 | N 8 5 8 5
// each strain followed by the tricks for declarers N E S W. Blank lines and lines starting
// with % or # are skipped; other PBN tags are ignored.
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "ddfast.hpp"
#include "port.hpp"

namespace {

const char* kSeats = "NESW";
const char* kStrains = "SHDCN";

int seat_of(char c) {
  const char* p = strchr(kSeats, toupper(c));
  return c && p ? int(p - kSeats) : -1;
}
int strain_of(const std::string& s) {
  if (s.empty()) return -1;
  if (s == "NT" || s == "nt") return 4;
  if (s.size() != 1) return -1;
  const char* p = strchr(kStrains, toupper(s[0]));
  return p ? int(p - kStrains) : -1;
}
int rank_bit(char c) {
  static const char* r = "23456789TJQKA";
  const char* p = strchr(r, toupper(c));
  return c && p ? int(p - r) : -1;
}

// Parses "N:hand hand hand hand"; false on any error (13 cards a hand, 52 different cards).
bool parse_pbn(const std::string& pbn, ddfast::Deal& d, std::string& err) {
  memset(&d, 0, sizeof d);
  if (pbn.size() < 2 || pbn[1] != ':' || seat_of(pbn[0]) < 0) return err = "expected a seat letter and ':'", false;
  int seat = seat_of(pbn[0]);
  std::istringstream in(pbn.substr(2));
  std::string hand;
  uint16_t seen[4] = {0, 0, 0, 0};
  for (int h = 0; h < 4; h++, seat = (seat + 1) & 3) {
    if (!(in >> hand)) return err = "expected four hands", false;
    int suit = 0, cards = 0;
    for (size_t i = 0; i < hand.size(); i++) {
      const char c = hand[i];
      if (c == '.') {
        if (++suit > 3) return err = "too many suits in a hand", false;
        continue;
      }
      int b = rank_bit(c);
      if (c == '1' && i + 1 < hand.size() && hand[i + 1] == '0') b = 8, i++;
      if (b < 0) return err = std::string("bad card '") + c + "'", false;
      if (seen[suit] >> b & 1) return err = "a card appears twice", false;
      seen[suit] |= uint16_t(1u << b);
      d.hand[seat][suit] |= uint16_t(1u << b);
      cards++;
    }
    if (suit != 3) return err = "a hand needs four suits", false;
    if (cards != 13) return err = "every hand needs 13 cards", false;
  }
  return true;
}

struct Line {
  std::string pbn;
  ddfast::Deal deal;
  int strain = -1, declarer = -1;
};

}  // namespace

// --cpu: the build's code paths and what this CPU offers.
static int cpu_report() {
  printf("build: %s\n", ddport::build_paths());
#if DDPORT_X86
  __builtin_cpu_init();
  const bool avx512 = __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512vl") &&
                      __builtin_cpu_supports("avx512bw") && __builtin_cpu_supports("avx512dq") &&
                      __builtin_cpu_supports("avx512vbmi2") && __builtin_cpu_supports("avx512vpopcntdq") &&
                      __builtin_cpu_supports("avx512bitalg") && __builtin_cpu_supports("gfni");
  printf("cpu: AVX-512 (Ice Lake set) %s, AVX2 %s, BMI2 %s\n", avx512 ? "yes" : "no",
         __builtin_cpu_supports("avx2") ? "yes" : "no", __builtin_cpu_supports("bmi2") ? "yes" : "no");
  if (avx512 && !DDPORT_AVX512) printf("note: this CPU could run the AVX-512 build (faster): build with -march=native\n");
#else
  printf("cpu: AArch64 (Highway NEON)\n");
#endif
  const char* m = ddport::cpu_check();
  printf("%s\n", m ? m : "ok: this CPU runs this build");
  return m ? 1 : 0;
}

int main(int argc, char** argv) {
  int threads = 1, def_strain = -1, def_decl = -1;
  const char* dir = nullptr;
  std::vector<std::string> files;
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        fprintf(stderr, "ddfast: %s needs a value\n", a.c_str());
        exit(2);
      }
      return argv[++i];
    };
    if (a == "-t" || a == "--threads") threads = atoi(next().c_str());
    else if (a == "-d" || a == "--dir") { next(); dir = argv[i]; }
    else if (a == "-s" || a == "--strain") def_strain = strain_of(next());
    else if (a == "-l" || a == "--declarer") def_decl = seat_of(next()[0]);
    else if (a == "--cpu") return cpu_report();
    else if (a == "-h" || a == "--help") {
      fprintf(stderr, "usage: ddfast [-t threads] [-d table dir] [-s strain] [-l declarer] [--cpu] [file ...]\n");
      return 0;
    } else files.push_back(a);
  }
  std::vector<Line> lines;
  auto read_stream = [&](std::istream& in, const std::string& name) {
    std::string s;
    int no = 0;
    while (std::getline(in, s)) {
      no++;
      if (!s.empty() && s.back() == '\r') s.pop_back();
      size_t b = s.find_first_not_of(" \t");
      if (b == std::string::npos || s[b] == '%' || s[b] == '#') continue;
      s = s.substr(b);
      if (s[0] == '[') {  // a PBN tag: only [Deal "..."] counts
        if (s.compare(0, 6, "[Deal ") != 0) continue;
        const size_t q1 = s.find('"'), q2 = s.rfind('"');
        if (q1 == std::string::npos || q2 <= q1) continue;
        s = s.substr(q1 + 1, q2 - q1 - 1) + s.substr(s.find(']', q2) + 1);
      }
      std::istringstream in2(s);
      std::vector<std::string> tok;
      for (std::string t; in2 >> t;) tok.push_back(t);
      Line ln;
      if (tok.size() < 4) {
        fprintf(stderr, "ddfast: %s:%d: expected a deal\n", name.c_str(), no);
        exit(1);
      }
      ln.pbn = tok[0] + " " + tok[1] + " " + tok[2] + " " + tok[3];
      std::string err;
      if (!parse_pbn(ln.pbn, ln.deal, err)) {
        fprintf(stderr, "ddfast: %s:%d: %s\n", name.c_str(), no, err.c_str());
        exit(1);
      }
      ln.strain = tok.size() > 4 ? strain_of(tok[4]) : def_strain;
      ln.declarer = tok.size() > 5 ? seat_of(tok[5][0]) : def_decl;
      if ((tok.size() > 4 && ln.strain < 0) || (tok.size() > 5 && ln.declarer < 0) || (ln.strain >= 0) != (ln.declarer >= 0)) {
        fprintf(stderr, "ddfast: %s:%d: give both a strain (S H D C N) and a declarer (N E S W), or neither\n", name.c_str(), no);
        exit(1);
      }
      lines.push_back(ln);
    }
  };
  if (files.empty()) read_stream(std::cin, "stdin");
  for (const auto& f : files) {
    std::ifstream in(f);
    if (!in) {
      fprintf(stderr, "ddfast: cannot read %s\n", f.c_str());
      return 1;
    }
    read_stream(in, f);
  }
  if (lines.empty()) return 0;
  ddfast::init(dir);

  // Single results and tables are solved as two batches, then printed in input order.
  std::vector<ddfast::Problem> probs;
  std::vector<ddfast::Deal> deals;
  for (const auto& ln : lines) {
    if (ln.strain >= 0) {
      ddfast::Problem p;
      memcpy(p.hand, ln.deal.hand, sizeof p.hand);
      p.trump = uint8_t(ln.strain);
      p.declarer = uint8_t(ln.declarer);
      probs.push_back(p);
    } else {
      deals.push_back(ln.deal);
    }
  }
  std::vector<uint8_t> tricks(probs.size());
  std::vector<ddfast::Table> tables(deals.size());
  std::vector<int> cpus = ddfast::available_cpus();
  if (threads > 0 && size_t(threads) < cpus.size()) cpus.resize(size_t(threads));
  if (threads == 1 || cpus.size() <= 1) {
    ddfast::solve(probs.data(), probs.size(), tricks.data());
    ddfast::solve_tables(deals.data(), deals.size(), tables.data());
  } else {
    if (!probs.empty()) ddfast::solve_parallel(probs.data(), probs.size(), tricks.data(), cpus.data(), int(cpus.size()));
    if (!deals.empty()) ddfast::solve_tables_parallel(deals.data(), deals.size(), tables.data(), cpus.data(), int(cpus.size()));
    ddfast::stop_pool();
  }
  size_t ip = 0, id = 0;
  for (const auto& ln : lines) {
    if (ln.strain >= 0) {
      printf("%s %c %c %d\n", ln.pbn.c_str(), kStrains[ln.strain], kSeats[ln.declarer], tricks[ip++]);
    } else {
      const ddfast::Table& t = tables[id++];
      printf("%s", ln.pbn.c_str());
      for (int st = 0; st < 5; st++) printf(" | %c %d %d %d %d", kStrains[st], t.tricks[st][0], t.tricks[st][1], t.tricks[st][2], t.tricks[st][3]);
      printf("\n");
    }
  }
  return 0;
}
