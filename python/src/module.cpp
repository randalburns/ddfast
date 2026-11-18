// The ddfast Python module (nanobind): DDS 3.x's dds3 interface (solve_board_pbn,
// calc_all_tables_pbn, SolverContext) on ddfast, plus solve_boards_pbn, a batch form of
// solve_board_pbn solved on ddfast's own threads. Argument names, defaults, checks, error types
// and messages, and result shapes follow dds3 (extern/dds/python/src/bindings.cpp,
// converters.cpp). The DDS semantics themselves are in dds_compat.cpp.
#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <atomic>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "dds_compat.hpp"
#include "ddfast.hpp"

namespace nb = nanobind;
using namespace nb::literals;
using ddfast_compat::Future;

namespace {

// dds3's throw_on_dds_error: ValueError for malformed input, RuntimeError otherwise.
[[noreturn]] void throw_dds(int code, const std::string& where = "") {
  const std::string text = where + "DDS error " + std::to_string(code) + ": " + ddfast_compat::error_text(code);
  switch (code) {
    case RETURN_TRUMP_WRONG:
    case RETURN_FIRST_WRONG:
    case RETURN_PBN_FAULT:
    case RETURN_TARGET_WRONG_LO:
    case RETURN_TARGET_WRONG_HI:
    case RETURN_SOLNS_WRONG_LO:
    case RETURN_SOLNS_WRONG_HI:
    case RETURN_THREAD_INDEX:
    case RETURN_MODE_WRONG_LO:
    case RETURN_MODE_WRONG_HI:
    case RETURN_NO_SUIT:
    case RETURN_TOO_MANY_TABLES:
    case RETURN_CARD_COUNT:
    case RETURN_DUPLICATE_CARDS:
    case RETURN_SUIT_OR_RANK:
    case RETURN_PAR_TABLE_FAULT:
      throw nb::value_error(text.c_str());
    default:
      throw std::runtime_error(text);
  }
}

// dds3's sequence_to_bounded_int_vector.
std::vector<int> bounded_ints(nb::handle seq, size_t n, int lo, int hi, const char* name) {
  if (!nb::isinstance<nb::sequence>(seq) || nb::len(seq) != n)
    throw nb::value_error((std::string(name) + " must have size " + std::to_string(n)).c_str());
  std::vector<int> v;
  for (size_t i = 0; i < n; i++) {
    const int x = nb::cast<int>(seq[i]);
    if (x < lo || x > hi)
      throw nb::value_error((std::string(name) + " has invalid value " + std::to_string(x) + " (expected range " +
                             std::to_string(lo) + ".." + std::to_string(hi) + ")")
                                .c_str());
    v.push_back(x);
  }
  return v;
}

// The checks dds3's pbn_to_deal makes before DDS sees the deal.
struct BoardArgs {
  int trump, first, ts[3], tr[3];
};
BoardArgs board_args(int trump, int first, nb::handle cts, nb::handle ctr) {
  if (trump < 0 || trump > 4)
    throw nb::value_error(("trump has invalid value " + std::to_string(trump) + " (expected range 0..4)").c_str());
  if (first < 0 || first > 3)
    throw nb::value_error(("first has invalid value " + std::to_string(first) + " (expected range 0..3)").c_str());
  const auto s = bounded_ints(cts, 3, 0, 3, "current_trick_suit");
  const auto r = bounded_ints(ctr, 3, 0, 14, "current_trick_rank");
  for (int x : r)
    if (x != 0 && (x < 2 || x > 14))
      throw nb::value_error(("current_trick_rank has invalid value " + std::to_string(x) + " (expected 0 or 2..14)").c_str());
  BoardArgs a{trump, first, {s[0], s[1], s[2]}, {r[0], r[1], r[2]}};
  return a;
}
void check_pbn_length(const std::string& pbn) {
  if (pbn.size() > 79)
    throw nb::value_error(("remain_cards PBN string is too long (" + std::to_string(pbn.size()) + " bytes, maximum 79)").c_str());
}

nb::tuple tuple13(const int* v) {
  PyObject* t = PyTuple_New(13);
  for (Py_ssize_t i = 0; i < 13; i++) PyTuple_SetItem(t, i, PyLong_FromLong(v[i]));
  return nb::steal<nb::tuple>(t);
}

// dds3's future_tricks_to_dict.
nb::dict future_dict(const Future& f) {
  nb::dict d;
  d["nodes"] = f.nodes;
  d["cards"] = f.cards;
  d["suit"] = tuple13(f.suit);
  d["rank"] = tuple13(f.rank);
  d["equals"] = tuple13(f.equals);
  d["score"] = tuple13(f.score);
  return d;
}

// dds3's SolverContext holds a DDS transposition table between calls. ddfast keeps its search
// state per thread and reuses it by itself when DDS would (same deal and strain, or mode 2), so
// the context carries nothing; it is accepted for compatibility.
struct SolverContext {};

}  // namespace

NB_MODULE(_ddfast, m) {
  m.doc() =
      "ddfast: an exact double-dummy solver. The functions follow DDS 3.x's dds3 module "
      "(solve_board_pbn, calc_all_tables_pbn, SolverContext); solve_boards_pbn is a batch form "
      "of solve_board_pbn.";

  nb::class_<SolverContext>(m, "SolverContext",
                            "Accepted for compatibility with dds3. ddfast keeps its search table per "
                            "thread and reuses it when DDS would, so a context carries no state.")
      .def(nb::init<>(), "Create a new solver context.");

  m.def(
      "init",
      [](nb::object table_dir, bool map_tables) {
        std::string dir = table_dir.is_none() ? std::string() : nb::cast<std::string>(table_dir);
        nb::gil_scoped_release release;
        ddfast_compat::ensure_init(dir.empty() ? nullptr : dir.c_str(), map_tables);
      },
      "table_dir"_a = nb::none(), "map_tables"_a = false,
      "Load the endgame tables (about 1.2 GB) from table_dir, else $DDFAST_TABLES, else build/tables "
      "under the current directory, building them there first if missing (a few minutes, once). "
      "Called automatically by the first solve; call it yourself to choose the directory or the "
      "moment. map_tables=True (or DDFAST_TABLE_MAP=1 in the environment) maps the table files "
      "instead of reading them in, so that all processes using them share one copy.");

  m.def(
      "solve_board_pbn",
      [](const std::string& remain_cards, int trump, int first, nb::handle current_trick_suit,
         nb::handle current_trick_rank, int target, int solutions, int mode, int thread_index,
         nb::handle context) {
        (void)thread_index;
        if (!context.is_none() && !nb::isinstance<SolverContext>(context))
          throw nb::type_error("context must be a SolverContext or None");
        const BoardArgs a = board_args(trump, first, current_trick_suit, current_trick_rank);
        check_pbn_length(remain_cards);
        Future f;
        int code;
        {
          nb::gil_scoped_release release;
          ddfast_compat::ensure_init();
          code = ddfast_compat::solve_board_pbn(remain_cards.c_str(), a.trump, a.first, a.ts, a.tr, target, solutions,
                                                mode, f);
        }
        if (code != RETURN_NO_FAULT) throw_dds(code);
        return future_dict(f);
      },
      "remain_cards"_a, "trump"_a = 4, "first"_a = 0, "current_trick_suit"_a = nb::make_tuple(0, 0, 0),
      "current_trick_rank"_a = nb::make_tuple(0, 0, 0), "target"_a = -1, "solutions"_a = 3, "mode"_a = 0,
      "thread_index"_a = 0, "context"_a = nb::none(),
      "Solve one deal from PBN, as dds3.solve_board_pbn (DDS's SolveBoardPBN).\n\n"
      "remain_cards: the cards still held, 'N:AK.234.456.789T ...' (only the first hand may have a "
      "compass letter). trump 0..3 spades..clubs, 4 no trumps. first: the seat that led the current "
      "trick (0 N, 1 E, 2 S, 3 W). current_trick_suit/rank: the cards already played to it (DDS reads "
      "as many as the card count implies). target -1: the best result; solutions 1: one best card, "
      "2: all best cards, 3: every card with its score.\n\n"
      "Returns dds3's dict: nodes (0 in ddfast), cards, and 13-tuples suit, rank, equals, score; "
      "scores are tricks for the side to play. Differences from DDS: among equally good cards, which "
      "one is reported first (solutions 1) and the order of equal scores may differ. thread_index "
      "and context are accepted and ignored (ddfast's search state is per thread). Releases the GIL, "
      "so calls from several threads run in parallel.");

  m.def(
      "solve_boards_pbn",
      [](const std::vector<std::string>& deals, int trump, int first, nb::handle current_trick_suit,
         nb::handle current_trick_rank, int target, int solutions, int mode, int max_threads) {
        const BoardArgs a = board_args(trump, first, current_trick_suit, current_trick_rank);
        for (const auto& d : deals) check_pbn_length(d);
        const size_t n = deals.size();
        std::vector<Future> out(n);
        std::vector<int> codes(n, RETURN_NO_FAULT);
        {
          nb::gil_scoped_release release;
          ddfast_compat::ensure_init();
          struct Job {
            const std::vector<std::string>* deals;
            const BoardArgs* a;
            int target, solutions, mode;
            std::vector<Future>* out;
            std::vector<int>* codes;
          } job{&deals, &a, target, solutions, mode, &out, &codes};
          ddfast_compat::parallel_for(n, max_threads, [](size_t i, void* p) {
            const Job& j = *static_cast<const Job*>(p);
            (*j.codes)[i] = ddfast_compat::solve_board_pbn((*j.deals)[i].c_str(), j.a->trump, j.a->first, j.a->ts,
                                                           j.a->tr, j.target, j.solutions, j.mode, (*j.out)[i]);
          }, &job);
        }
        for (size_t i = 0; i < n; i++)
          if (codes[i] != RETURN_NO_FAULT) throw_dds(codes[i], "deal " + std::to_string(i) + ": ");
        nb::list r;
        for (const Future& f : out) r.append(future_dict(f));
        return r;
      },
      "deals"_a, "trump"_a = 4, "first"_a = 0, "current_trick_suit"_a = nb::make_tuple(0, 0, 0),
      "current_trick_rank"_a = nb::make_tuple(0, 0, 0), "target"_a = -1, "solutions"_a = 3, "mode"_a = 0,
      "max_threads"_a = 0,
      "solve_board_pbn for every PBN deal in `deals` (same strain, leader, cards of the current trick, "
      "target, solutions and mode), on max_threads threads (0: all CPUs), returning a list of dds3 "
      "result dicts in the same order. One call per batch instead of one per deal: the GIL is "
      "released once and no Python thread pool is needed. Not in dds3. Raises on the first deal (in "
      "list order) that DDS would reject, naming its index.");

  m.def(
      "calc_all_tables_pbn",
      [](nb::list deals_pbn, int mode, nb::handle trump_filter, int max_threads) {
        if (mode < -1 || mode > 3)
          throw nb::value_error(("mode has invalid value " + std::to_string(mode) +
                                 " (expected -1=disabled, 0=none, 1=both, 2=NS, 3=EW)")
                                    .c_str());
        const auto filt = bounded_ints(trump_filter, 5, 0, 1, "trump_filter");
        int included = 0;
        for (int x : filt) included += x == 0;
        const bool wants_par = mode != -1;
        if (wants_par && included != 5)
          throw nb::value_error(
              "Par computation (mode != -1) requires all strains to be included (trump_filter must be all zeros)");
        const size_t n = nb::len(deals_pbn);
        std::vector<std::string> pbn(n);
        for (size_t i = 0; i < n; i++) {
          pbn[i] = nb::cast<std::string>(deals_pbn[i]);
          if (pbn[i].size() >= 80)
            throw nb::value_error(("PBN string at index " + std::to_string(i) + " is too long (max 79 characters)").c_str());
        }
        struct Hands {
          uint16_t h[4][4];
        };
        std::vector<Hands> hands(n);
        std::vector<DdTableResults> res(n);
        std::vector<ParResults> par(wants_par ? n : 0);
        int filter[5];
        for (int s = 0; s < 5; s++) filter[s] = filt[size_t(s)];
        int code = RETURN_NO_FAULT;
        {
          nb::gil_scoped_release release;
          for (size_t i = 0; i < n && code == RETURN_NO_FAULT; i++) code = ddfast_compat::parse_table_deal(pbn[i].c_str(), hands[i].h);
          if (code == RETURN_NO_FAULT && n > 0) {
            ddfast_compat::ensure_init();
            code = ddfast_compat::calc_tables(&hands[0].h, n, filter, max_threads, res.data());
          }
          for (size_t i = 0; i < par.size() && code == RETURN_NO_FAULT; i++) {
            const int rc = ddfast_compat::par(res[i], mode, par[i]);
            if (rc != RETURN_NO_FAULT) code = rc;
          }
        }
        if (code != RETURN_NO_FAULT) throw_dds(code);
        nb::dict result;
        result["no_of_boards"] = int(4 * included * n);
        nb::list tables;
        for (size_t i = 0; i < n; i++) {
          nb::list rows;
          for (int st = 0; st < 5; st++) {
            nb::list row;
            for (int h = 0; h < 4; h++) row.append(res[i].res_table[st][h]);
            rows.append(row);
          }
          nb::dict t;
          t["res_table"] = rows;
          tables.append(t);
        }
        result["tables"] = tables;
        nb::list pars;
        for (const ParResults& p : par) {
          nb::dict d;
          nb::list s;
          s.append(std::string(p.par_score[0]));
          s.append(std::string(p.par_score[1]));
          d["par_score"] = s;
          nb::list c;
          c.append(std::string(p.par_contracts_string[0]));
          c.append(std::string(p.par_contracts_string[1]));
          d["par_contracts_string"] = c;
          pars.append(d);
        }
        result["par_results"] = pars;
        return result;
      },
      "deals_pbn"_a, "mode"_a = -1, "trump_filter"_a = nb::make_tuple(0, 0, 0, 0, 0), "max_threads"_a = 0,
      "Double-dummy tables (and par) for PBN deals, as dds3.calc_all_tables_pbn (DDS's "
      "CalcAllTablesPBN).\n\n"
      "mode -1: no par; 0..3: par with vulnerability none, both, NS, EW. trump_filter: 1 leaves a "
      "strain out (its entries are 0); par needs all strains. max_threads 0: all CPUs.\n\n"
      "Returns dds3's dict: no_of_boards, tables (each {'res_table': 5x4, [strain][hand], declarer's "
      "tricks; strains S H D C NT, hands N E S W}), par_results (each {'par_score': [NS, EW], "
      "'par_contracts_string': [NS, EW]}, DDS's own text; empty without par). Unlike dds3 there is no "
      "limit on the number of deals. Full deals are solved on ddfast's thread pool; hands of fewer "
      "than 13 cards position by position.");
}
