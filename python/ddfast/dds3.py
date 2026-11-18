"""DDS 3.x's ``dds3`` interface on ddfast: ``import ddfast.dds3 as dds3``.

Covers ``solve_board_pbn``, ``calc_all_tables_pbn`` and ``SolverContext`` with dds3's
arguments, defaults, errors and result dicts. Differences: ``nodes`` is 0; among equally good
cards, which one ``solve_board_pbn`` reports first (solutions 1) and the order of equal scores
may differ from DDS; ``thread_index`` and ``context`` are accepted and ignored (ddfast keeps
its search state per thread); ``calc_all_tables_pbn`` has no limit on the number of deals.
``solve_boards_pbn`` (a batch of deals in one call, on ddfast's threads) is an addition.
"""
from ._ddfast import SolverContext, calc_all_tables_pbn, init, solve_board_pbn, solve_boards_pbn

__all__ = ["SolverContext", "calc_all_tables_pbn", "init", "solve_board_pbn", "solve_boards_pbn"]
