"""Tests of the ddfast Python module's dds3 interface.

    PYTHONPATH=python DDFAST_TABLES=... python3 python/tests/test_dds3.py [deals]

The DDS semantics themselves are checked against DDS in C++ (python/tests/compat_test.cpp);
this checks the binding: result shapes, errors, answers against tests/data/public.set (DDS's
answers), the batch call against the single-board call, and calls from several threads.
"""
import os
import struct
import sys
from concurrent.futures import ThreadPoolExecutor

import ddfast
import ddfast.dds3 as dds3

ROOT = os.path.join(os.path.dirname(__file__), "..", "..")
RANKS = "23456789TJQKA"


def read_set(path):
    """tests/data/public.set: n, then n problems (hand[4][4] uint16, trump, declarer), n answers."""
    data = open(path, "rb").read()
    n = struct.unpack_from("<I", data, 0)[0]
    probs, off = [], 4
    for _ in range(n):
        hands = struct.unpack_from("<16H", data, off)
        trump, declarer = data[off + 32], data[off + 33]
        probs.append(([hands[4 * h:4 * h + 4] for h in range(4)], trump, declarer))
        off += 34
    return probs, list(data[off:off + n])


def pbn(hands):
    """N:... with suits S H D C; bit r-2 of hand[seat][suit] is rank r."""
    def suit(bits):
        return "".join(RANKS[r] for r in range(12, -1, -1) if bits >> r & 1)
    return "N:" + " ".join(".".join(suit(h[s]) for s in range(4)) for h in hands)


def check(cond, what):
    if not cond:
        raise AssertionError(what)


def test_shapes(deal):
    r = dds3.solve_board_pbn(deal, trump=4, first=1)
    check(set(r) == {"nodes", "cards", "suit", "rank", "equals", "score"}, "solve_board_pbn keys")
    for k in ("suit", "rank", "equals", "score"):
        check(isinstance(r[k], tuple) and len(r[k]) == 13, k + " is a 13-tuple")
    check(1 <= r["cards"] <= 13, "cards")
    check(list(r["score"][:r["cards"]]) == sorted(r["score"][:r["cards"]], reverse=True), "solutions 3 sorted")
    t = dds3.calc_all_tables_pbn([deal, deal], mode=1)
    check(t["no_of_boards"] == 40 and len(t["tables"]) == 2 and len(t["par_results"]) == 2, "tables shape")
    check(len(t["tables"][0]["res_table"]) == 5 and all(len(x) == 4 for x in t["tables"][0]["res_table"]), "res_table 5x4")
    p = t["par_results"][0]
    check(p["par_score"][0].startswith("NS ") and p["par_score"][1].startswith("EW "), "par_score text")
    check(dds3.calc_all_tables_pbn([deal])["par_results"] == [], "no par without mode")
    f = dds3.calc_all_tables_pbn([deal], trump_filter=(0, 1, 0, 1, 0))
    check(f["no_of_boards"] == 12 and f["tables"][0]["res_table"][1] == [0, 0, 0, 0], "trump_filter")


def test_errors(deal):
    cases = [
        (lambda: dds3.solve_board_pbn("N:N:" + deal[2:]), ValueError, "DDS error -99"),
        (lambda: dds3.solve_board_pbn(deal, trump=5), ValueError, "trump has invalid value 5"),
        (lambda: dds3.solve_board_pbn(deal, first=-1), ValueError, "first has invalid value -1"),
        (lambda: dds3.solve_board_pbn(deal, current_trick_suit=(0, 0)), ValueError, "must have size 3"),
        (lambda: dds3.solve_board_pbn(deal, current_trick_rank=(1, 0, 0)), ValueError, "expected 0 or 2..14"),
        (lambda: dds3.solve_board_pbn(deal, solutions=4), ValueError, "DDS error -9"),
        (lambda: dds3.solve_board_pbn(deal, target=14), ValueError, "DDS error -7"),
        (lambda: dds3.solve_board_pbn(deal, mode=3), ValueError, "DDS error -17"),
        (lambda: dds3.solve_board_pbn(deal[:-1]), ValueError, "DDS error -14"),
        (lambda: dds3.calc_all_tables_pbn([deal], mode=4), ValueError, "mode has invalid value 4"),
        (lambda: dds3.calc_all_tables_pbn([deal], mode=0, trump_filter=(1, 0, 0, 0, 0)), ValueError, "requires all strains"),
        (lambda: dds3.solve_boards_pbn([deal, "N:N:"]), ValueError, "deal 1: DDS error -99"),
    ]
    for f, exc, text in cases:
        try:
            f()
        except exc as e:
            check(text in str(e), "error text: expected %r in %r" % (text, str(e)))
        else:
            raise AssertionError("no %s for %s" % (exc.__name__, text))


def test_answers(probs, answers, n):
    deals = [pbn(h) for h, _, _ in probs[:n]]
    for i, (h, trump, declarer) in enumerate(probs[:n]):
        r = dds3.solve_board_pbn(deals[i], trump=trump, first=(declarer + 1) % 4, solutions=1, mode=1)
        check(13 - r["score"][0] == answers[i], "deal %d: %d vs DDS %d" % (i, 13 - r["score"][0], answers[i]))
    # The batch call, one strain and leader per call as BEN groups its samples.
    by = {}
    for i, (h, trump, declarer) in enumerate(probs[:n]):
        by.setdefault((trump, (declarer + 1) % 4), []).append(i)
    for (trump, first), idx in by.items():
        rs = dds3.solve_boards_pbn([deals[i] for i in idx], trump=trump, first=first, solutions=1, mode=1, max_threads=4)
        for i, r in zip(idx, rs):
            check(13 - r["score"][0] == answers[i], "batch deal %d" % i)
    # Several Python threads at once, as BEN's DDSolver does.
    def one(i):
        h, trump, declarer = probs[i]
        return 13 - dds3.solve_board_pbn(deals[i], trump=trump, first=(declarer + 1) % 4, solutions=1, mode=1,
                                         context=dds3.SolverContext())["score"][0]
    with ThreadPoolExecutor(8) as ex:
        got = list(ex.map(one, range(n)))
    check(got == answers[:n], "threaded answers")
    # solutions 3 and 2 agree with solutions 1.
    for i in range(min(n, 50)):
        h, trump, declarer = probs[i]
        a = dds3.solve_board_pbn(deals[i], trump=trump, first=(declarer + 1) % 4, solutions=3)
        b = dds3.solve_board_pbn(deals[i], trump=trump, first=(declarer + 1) % 4, solutions=2, mode=1)
        check(a["score"][0] == 13 - answers[i] and all(s == a["score"][0] for s in b["score"][:b["cards"]]), "sol 2/3")


def test_tables(probs):
    deals = [pbn(h) for h, _, _ in probs[:60]]
    t = dds3.calc_all_tables_pbn(deals, max_threads=4)
    for i, (h, trump, declarer) in enumerate(probs[:60]):
        r = dds3.solve_board_pbn(deals[i], trump=trump, first=(declarer + 1) % 4, solutions=1, mode=1)
        check(t["tables"][i]["res_table"][trump][declarer] == 13 - r["score"][0], "table entry %d" % i)


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 300
    probs, answers = read_set(os.path.join(ROOT, "tests", "data", "public.set"))
    ddfast.init()
    deal = pbn(probs[0][0])
    test_shapes(deal)
    test_errors(deal)
    test_answers(probs, answers, n)
    test_tables(probs)
    print("test_dds3: shapes, errors, %d answers (single, batch, threaded), tables: all correct" % n)


if __name__ == "__main__":
    main()
