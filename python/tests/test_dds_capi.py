"""libdds.so (DDS's C interface on ddfast) against a real DDS library, through ctypes.

    python3 python/tests/test_dds_capi.py <reference libdds> [positions] [seed]

Loads both libraries in one process (each with its own symbol scope) and solves the same
random positions with each: random deals with 0-12 tricks removed and 0-3 cards into the
current trick (revokes included, which DDS accepts), every strain and leader, solutions 1-3,
target -1 and fixed targets, modes 0-2, both SolveBoardPBN and SolveBoard. Then the same from
8 threads at once with thread indexes 0-7, as a multi-threaded caller (BGADLL) uses DDS.

Agreement is DDS's contract: the return code, the number of cards, the scores, and the cards
with their `equals`, except that with solutions 1 the reported card may be another equally good
one, and the order of cards with equal scores may differ.
"""
import ctypes
import os
import random
import sys
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
OURS = os.path.join(HERE, "..", "ddfast", "libdds.so")


class DealPBN(ctypes.Structure):
    _fields_ = [("trump", ctypes.c_int), ("first", ctypes.c_int),
                ("currentTrickSuit", ctypes.c_int * 3), ("currentTrickRank", ctypes.c_int * 3),
                ("remainCards", ctypes.c_char * 80)]


class Deal(ctypes.Structure):
    _fields_ = [("trump", ctypes.c_int), ("first", ctypes.c_int),
                ("currentTrickSuit", ctypes.c_int * 3), ("currentTrickRank", ctypes.c_int * 3),
                ("remainCards", (ctypes.c_uint * 4) * 4)]


class FutureTricks(ctypes.Structure):
    _fields_ = [("nodes", ctypes.c_int), ("cards", ctypes.c_int), ("suit", ctypes.c_int * 13),
                ("rank", ctypes.c_int * 13), ("equals", ctypes.c_int * 13), ("score", ctypes.c_int * 13)]


def load(path):
    lib = ctypes.CDLL(os.path.abspath(path), mode=os.RTLD_LOCAL)
    lib.SolveBoardPBN.argtypes = [DealPBN, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                  ctypes.POINTER(FutureTricks), ctypes.c_int]
    lib.SolveBoard.argtypes = [Deal, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                               ctypes.POINTER(FutureTricks), ctypes.c_int]
    lib.SolveBoardPBN.restype = lib.SolveBoard.restype = ctypes.c_int
    lib.SetMaxThreads.argtypes = [ctypes.c_int]
    lib.SetMaxThreads.restype = None
    lib.ErrorMessage.argtypes = [ctypes.c_int, ctypes.c_char_p]
    lib.ErrorMessage.restype = None
    return lib


RANKS = "23456789TJQKA"  # rank r (2..14) is RANKS[r - 2]


def random_position(rng):
    """(trump, first, trick_suits, trick_ranks, hands) with hands[h][s] a set of ranks 2..14."""
    cards = [(s, r) for s in range(4) for r in range(2, 15)]
    rng.shuffle(cards)
    hands = [[set() for _ in range(4)] for _ in range(4)]
    for i, (s, r) in enumerate(cards):
        hands[i // 13][s].add(r)
    for _ in range(rng.randint(0, 12)):           # whole tricks gone
        for h in range(4):
            held = [(s, r) for s in range(4) for r in hands[h][s]]
            s, r = rng.choice(held)
            hands[h][s].discard(r)
    first = rng.randrange(4)
    n_trick = rng.randint(0, 3)
    ts, tr = [0, 0, 0], [0, 0, 0]
    for i in range(n_trick):                       # cards into the current trick (any card)
        h = (first + i) % 4
        held = [(s, r) for s in range(4) for r in hands[h][s]]
        s, r = rng.choice(held)
        hands[h][s].discard(r)
        ts[i], tr[i] = s, r
    return rng.randrange(5), first, ts, tr, hands


def pbn(hands):
    return "N:" + " ".join(".".join("".join(RANKS[r - 2] for r in sorted(hands[h][s], reverse=True))
                                    for s in range(4)) for h in range(4))


def binary(hands):
    remain = ((ctypes.c_uint * 4) * 4)()
    for h in range(4):
        for s in range(4):
            remain[h][s] = sum(1 << r for r in hands[h][s])
    return remain


def solve(lib, pos, target, solutions, mode, use_pbn, thread_index):
    trump, first, ts, tr, hands = pos
    fut = FutureTricks()
    if use_pbn:
        d = DealPBN(trump, first, (ctypes.c_int * 3)(*ts), (ctypes.c_int * 3)(*tr), pbn(hands).encode())
        rc = lib.SolveBoardPBN(d, target, solutions, mode, ctypes.byref(fut), thread_index)
    else:
        d = Deal(trump, first, (ctypes.c_int * 3)(*ts), (ctypes.c_int * 3)(*tr), binary(hands))
        rc = lib.SolveBoard(d, target, solutions, mode, ctypes.byref(fut), thread_index)
    cards = [(fut.suit[i], fut.rank[i], fut.equals[i], fut.score[i]) for i in range(max(0, fut.cards))]
    return rc, fut.cards, cards


def agree(ref, got, solutions):
    if ref[0] != got[0]:
        return False
    if ref[0] != 1:                                # an error: the code is the answer
        return True
    if ref[1] != got[1]:
        return False
    if solutions == 1:                             # one best card: its score must agree
        return [c[3] for c in ref[2]] == [c[3] for c in got[2]]
    return sorted(ref[2]) == sorted(got[2])


def cases(rng, n):
    for _ in range(n):
        pos = random_position(rng)
        for solutions in (1, 2, 3):
            for target in (-1, rng.randint(0, 13)):
                yield pos, target, solutions, rng.choice((0, 1, 2)), rng.random() < 0.5


def run(ref, ours, work, thread_index, failures):
    for pos, target, solutions, mode, use_pbn in work:
        a = solve(ref, pos, target, solutions, mode, use_pbn, thread_index)
        b = solve(ours, pos, target, solutions, mode, use_pbn, thread_index)
        if not agree(a, b, solutions):
            failures.append((pbn(pos[4]), pos[:4], target, solutions, mode, use_pbn, a, b))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    ref, ours = load(sys.argv[1]), load(OURS)
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 2000
    seed = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    rng = random.Random(seed)
    ref.SetMaxThreads(8)
    ours.SetMaxThreads(8)

    for code in (1, -2, -12, -99):
        a, b = ctypes.create_string_buffer(80), ctypes.create_string_buffer(80)
        ref.ErrorMessage(code, a)
        ours.ErrorMessage(code, b)
        assert a.value == b.value, (code, a.value, b.value)

    work = list(cases(rng, n))
    failures = []
    run(ref, ours, work, 0, failures)
    single = len(work)

    work = list(cases(rng, n))
    threads = [threading.Thread(target=run, args=(ref, ours, work[i::8], i, failures)) for i in range(8)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    total = single + len(work)
    if failures:
        print(f"test_dds_capi: {len(failures)} of {total} calls differ")
        for f in failures[:10]:
            print("  ", f)
        sys.exit(1)
    print(f"test_dds_capi: {total} calls ({2 * n} positions, single and 8 threads) agree with {sys.argv[1]}")


if __name__ == "__main__":
    main()
