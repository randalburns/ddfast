// DDS's legacy C interface (dll.h) on ddfast, built as a libdds replacement (libdds.so /
// libdds.dylib) for programs that load DDS by name, such as BGADLL (BEN's PIMC engine).
//
// Each call goes to ddfast_compat, which holds DDS's SolveBoard semantics (input checks and
// their order, return codes, special cases, the grouping of equivalent cards into `equals`).
// This file only converts the structs. It is linked against libddfast.so, which holds the
// solver, the endgame tables and the thread pool, so a process that also uses ddfast's Python
// module has one copy of the tables.
//
// Thread model: DDS 2.x takes a thread index (0 .. the count given to SetMaxThreads) and keeps
// a search table per index. ddfast keeps its search state per OS thread, so the index is not
// needed; any index is accepted and calls from any number of threads run in parallel.
// SetMaxThreads, SetResources and FreeMemory have nothing to set and do nothing.
//
// Covered: SetMaxThreads, SetResources, FreeMemory, SolveBoard, SolveBoardPBN, ErrorMessage.
// The tables are loaded on the first solve from $DDFAST_TABLES, else build/tables under the
// current directory (built there first if missing, as ddfast::init does).
#include <cstring>

#include <api/dll.h>

#include "dds_compat.hpp"

namespace {

void to_future(const ddfast_compat::Future& f, FutureTricks* out) {
  out->nodes = f.nodes;
  out->cards = f.cards;
  for (int i = 0; i < 13; i++) {
    out->suit[i] = f.suit[i];
    out->rank[i] = f.rank[i];
    out->equals[i] = f.equals[i];
    out->score[i] = f.score[i];
  }
}

}  // namespace

auto STDCALL SetMaxThreads(int userThreads) -> void { (void)userThreads; }

auto STDCALL SetResources(int maxMemoryMB, int maxThreads) -> void {
  (void)maxMemoryMB;
  (void)maxThreads;
}

auto STDCALL FreeMemory() -> void {}

auto STDCALL SolveBoard(Deal dl, int target, int solutions, int mode, FutureTricks* futp, int threadIndex)
    -> int {
  (void)threadIndex;
  if (futp == nullptr) return RETURN_UNKNOWN_FAULT;
  ddfast_compat::ensure_init();
  ddfast_compat::Future f;
  const int rc = ddfast_compat::solve_board(dl.remainCards, dl.trump, dl.first, dl.currentTrickSuit,
                                            dl.currentTrickRank, target, solutions, mode, f);
  to_future(f, futp);
  return rc;
}

auto STDCALL SolveBoardPBN(DealPBN dlpbn, int target, int solutions, int mode, FutureTricks* futp,
                           int thrId) -> int {
  (void)thrId;
  if (futp == nullptr) return RETURN_UNKNOWN_FAULT;
  // DDS reads remainCards as a C string within its 80 bytes.
  char pbn[81];
  std::memcpy(pbn, dlpbn.remainCards, 80);
  pbn[80] = '\0';
  ddfast_compat::ensure_init();
  ddfast_compat::Future f;
  const int rc = ddfast_compat::solve_board_pbn(pbn, dlpbn.trump, dlpbn.first, dlpbn.currentTrickSuit,
                                                dlpbn.currentTrickRank, target, solutions, mode, f);
  to_future(f, futp);
  return rc;
}

auto STDCALL ErrorMessage(int code, char line[80]) -> void {
  std::strncpy(line, ddfast_compat::error_text(code), 79);
  line[79] = '\0';
}
