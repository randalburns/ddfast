"""BEN-style batches through the Python module: one position, its hidden cards re-dealt per
sample, solved (a) one call at a time, (b) one call per sample on a thread pool (BEN's
DDSolver), (c) one solve_boards_pbn call.

    PYTHONPATH=python DDFAST_TABLES=... python3 python/tests/bench_ben.py [samples] [threads]
"""
import random
import sys
import time
from concurrent.futures import ThreadPoolExecutor

import ddfast
import ddfast.dds3 as dds3

RANKS = "23456789TJQKA"


def deal_after(rng, tricks_played, in_trick):
    """A random deal played out at random: hands (sets of (suit, rank)), leader, trick cards, trump."""
    deck = [(s, r) for s in range(4) for r in range(2, 15)]
    rng.shuffle(deck)
    hands = [set(deck[13 * h:13 * h + 13]) for h in range(4)]
    trump, leader, trick = rng.randrange(5), rng.randrange(4), []
    for _ in range(4 * tricks_played + in_trick):
        seat = (leader + len(trick)) % 4
        follow = [c for c in hands[seat] if trick and c[0] == trick[0][0]]
        card = rng.choice(sorted(follow or hands[seat]))
        hands[seat].remove(card)
        trick.append(card)
        if len(trick) == 4:
            best = 0
            for j in range(1, 4):
                c, b = trick[j], trick[best]
                if (trump < 4 and c[0] == trump and b[0] != trump) or (c[0] == b[0] and c[1] > b[1]):
                    best = j
            leader, trick = (leader + best) % 4, []
    return hands, leader, trick, trump


def pbn(hands):
    return "N:" + " ".join(".".join("".join(RANKS[r - 2] for r in range(14, 1, -1) if (s, r) in h) for s in range(4)) for h in hands)


def samples(rng, hands, n, known):
    """Re-deal the cards of the two seats not in `known` between them, keeping their counts."""
    hidden = [h for h in range(4) if h not in known]
    out = []
    for _ in range(n):
        pool = sorted(hands[hidden[0]] | hands[hidden[1]])
        rng.shuffle(pool)
        k = len(hands[hidden[0]])
        hs = [set(h) for h in hands]
        hs[hidden[0]], hs[hidden[1]] = set(pool[:k]), set(pool[k:])
        out.append(pbn(hs))
    return out


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 200
    threads = int(sys.argv[2]) if len(sys.argv) > 2 else 8
    rng = random.Random(11)
    ddfast.init()
    print("%d samples per batch, %d threads; ms per batch (best of 3)" % (n, threads))
    print("%-24s %4s %10s %12s %12s %8s" % ("position", "sol", "loop", "thread pool", "batch call", "batch/pool"))
    for tricks_played, in_trick in [(0, 1), (3, 0), (6, 2), (9, 1)]:
        hands, leader, trick, trump = deal_after(rng, tricks_played, in_trick)
        seat = (leader + len(trick)) % 4
        deals = samples(rng, hands, n, known={seat, (seat + 2) % 4})
        ts = tuple([c[0] for c in trick] + [0] * (3 - len(trick)))
        tr = tuple([c[1] for c in trick] + [0] * (3 - len(trick)))
        for sol in (1, 3):
            kw = dict(trump=trump, first=leader, current_trick_suit=ts, current_trick_rank=tr, target=-1, solutions=sol, mode=1)
            ref = [dds3.solve_board_pbn(d, **kw) for d in deals]
            def loop():
                return [dds3.solve_board_pbn(d, **kw) for d in deals]
            pool = ThreadPoolExecutor(threads)
            def pooled():
                return list(pool.map(lambda d: dds3.solve_board_pbn(d, context=dds3.SolverContext(), **kw), deals))
            def batch():
                return dds3.solve_boards_pbn(deals, max_threads=threads, **kw)
            t = {}
            for name, f in (("loop", loop), ("pool", pooled), ("batch", batch)):
                best = 1e9
                for _ in range(3):
                    t0 = time.perf_counter()
                    got = f()
                    best = min(best, time.perf_counter() - t0)
                assert [g["score"][:g["cards"]] for g in got] == [r["score"][:r["cards"]] for r in ref], name
                t[name] = best * 1e3
            pool.shutdown()
            what = "trick %d, %d in trick" % (tricks_played + 1, len(trick))
            print("%-24s %4d %10.2f %12.2f %12.2f %7.2fx" % (what, sol, t["loop"], t["pool"], t["batch"], t["pool"] / t["batch"]))


if __name__ == "__main__":
    main()
